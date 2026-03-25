/*
 * qwswayland.c - QWSWayland proxy daemon implementation
 * SPDX-License-Identifier: MIT
 */

#include "proxy.h"
#include "client.h"
#include "window.h"
#include "qws_server_helpers.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <fcntl.h>

#include <assert.h>


/* ================================================================
 * Command dispatch
 * ================================================================ */

void qwswl_handle_client_data(qwswl_state_t *state, qwswl_client_t *cl)
{
    uint8_t buf[4096];

    ssize_t n = read(cl->fd, buf, sizeof(buf));
    if (n <= 0) {
        qwswl_disconnect_client(state, cl);
        return;
    }

    /* Raw wire trace at max verbosity */
    qws_trace_raw_bytes(cl->client_id, buf, (size_t)n);

    size_t offset = 0;
    while (offset < (size_t)n) {
        qws_packet_t *pkt = NULL;
        size_t consumed = qws_reader_feed(&cl->reader,
                                            buf + offset,
                                            (size_t)n - offset,
                                            &pkt);
        if (consumed == 0) break;
        offset += consumed;

        if (pkt) {
            qws_trace_packet(cl->client_id, pkt, false);
            qwswl_dispatch_command(state, cl, pkt);
            qws_packet_free(pkt);
        }
    }
}

void qwswl_dispatch_command(qwswl_state_t *state, qwswl_client_t *cl,
                             qws_packet_t *incoming_pkt)
{
    assert(incoming_pkt);
    int32_t type = incoming_pkt->header.type;

    switch (type) {

    case QWS_CMD_IDENTIFY: {
        /* First command from client after connecting.
         * Contains the app name and the client's lock semaphore ID.
         * We need to attach to the client's lock so we can synchronize
         * Communication and RegionEvent signaling. */
        qws_cmd_identify_t *cmd = (qws_cmd_identify_t *)incoming_pkt->simple_data;
 
        /* Lock ID is in simpleData.id_lock */
        int32_t lock_id = cmd->id_lock;
 
        if (lock_id >= 0) {
            if (qws_lock_open(&cl->lock, state->ipc_type, lock_id) == 0) {
                fprintf(stderr, "[qwswayland] Client %d: attached to lock id=%d (%s)\n",
                        cl->client_id, lock_id,
                        state->ipc_type == QWS_IPC_SYSV ? "SysV" : "POSIX");
            } else {
                fprintf(stderr, "[qwswayland] Client %d: FAILED to attach lock id=%d\n",
                        cl->client_id, lock_id);
            }
        } else {
            fprintf(stderr, "[qwswayland] Client %d: error no lock id in Identify\n", cl->client_id);
        }

        /* Send Connected event */
        char display_spec[128];
        snprintf(display_spec, sizeof(display_spec), "vnc:size=%dx%d:depth=%d", state->screen_width, 
            state->screen_height, state->screen_depth);
        qws_packet_t *conn = qws_make_connected_event(
            cl->client_id, state->display_shm.shm_id, display_spec);
        qws_trace_packet(cl->client_id, conn, true);
        qws_write_packet(cl->fd, conn);

        /*
        * The client blocks in waitForCreation() after connecting and
        * will not proceed until it receives a Creation event. The server
        * must proactively provide IDs — the client does not send a
        * Create command first. Therefore, the first window is allocated
        * by default. */
        qwswl_window_t *window = qwswl_allocate_window(cl);
    
        qws_packet_t *cre = qws_make_creation_event(window->qws_id, 1);
        qws_trace_packet(cl->client_id, cre, true);
        qws_write_packet(cl->fd, cre);

        break;
    }

    case QWS_CMD_CREATE: {
        /* Client requests `count` new object IDs.
         * Server allocates a contiguous range and responds with a single
         * Creation event containing the first ID and the count.
         * These are generic IDs — the client will later use them for
         * windows, properties, etc. We don't create visible windows here. */
        qws_cmd_create_t *cmd = (qws_cmd_create_t *)incoming_pkt->simple_data;
        assert(cmd->count >= 0);
 
        int32_t first_id = cl->next_window_id + 1;
        for (int32_t i = 0; i < cmd->count; i++) {
            /* Create a window */
            assert(qwswl_allocate_window(cl));
        }
 
        qws_packet_t *evt = qws_make_creation_event(first_id, cmd->count);
        qws_trace_packet(cl->client_id, evt, true);
        qws_write_packet(cl->fd, evt);
        break;
    }

    case QWS_CMD_REGION: {
        /* Client requests a screen region for its window */
        qws_cmd_region_request_t *cmd =
            (qws_cmd_region_request_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        if (!win) {
            fprintf(stderr, "[qwswayland] Illegal window id %d from client %d\n",
                cmd->window, cl->fd);
            break;
        }

        /* Extract raw data */
        int32_t nrects = cmd->nrectangles;
        qws_rect_t *rects = (qws_rect_t *)incoming_pkt->raw_data;
        char * surface_key;
        uint8_t *surface_data = 
            &((uint8_t *) incoming_pkt->raw_data)[cmd->nrectangles * sizeof(qws_rect_t) + cmd->surfacekeylength * 2];

        if (nrects > 0) {
            if (qws_convert_to_narrow_unicode(&surface_key, (const wchar_t *)
                    &((char *) incoming_pkt->raw_data)[cmd->nrectangles * sizeof(qws_rect_t)],
                    cmd->surfacekeylength * 2) < 0) {
                fprintf(stderr, "[qwswayland] Illegal surface key from client %d\n",
                    cl->client_id);
                free(surface_key);
                break;
            }

            if (strcmp(surface_key, "shm") == 0 &&
                    cmd->surfacedatalength >= (int32_t)sizeof(qws_cmd_region_surface_data_shm_t)) {
                const qws_cmd_region_surface_data_shm_t *sd =
                    (const qws_cmd_region_surface_data_shm_t *)surface_data;

                assert(sd->mem_id >= 0);

                qwswl_attach_client_shm(win, sd->mem_id, sd->width, sd->height);
            }
            free(surface_key);

            /* We grant the full requested region and also
             * use it to size the Wayland surface. */
            qwswl_update_geometry(state, win, rects, nrects);

            /* If the surface for the window already exists, then 
             * we're able to actually display content. Otherwise, we
             * need to wait until we are being let know what type
             * of window we are actually dealing with. And when is 
             * the best time for that to happen:
             *  - during creation? No way.
             *  - during region definition? That's too easy.
             *  - during repaint? Yes, exactly there :-/ */
            if (win->wl_surface)
                qwswl_update_surface(state, win, rects, nrects);
        } else {
            /* The QWS client wants us to hide the surface temporarily. */
            qwswl_hide_window(state, win);
        }

        /* Send the region back as granted */
        qws_packet_t *evt = qws_make_region_event(
            cmd->window, 0 /* allocation */, rects, nrects);
        qws_trace_packet(cl->client_id, evt, true);
        qws_write_packet(cl->fd, evt);
        
        qws_lock_unlock(&cl->lock, QWS_LOCK_REGIONEVENT);

        break;
    }

    case QWS_CMD_REGION_NAME: {
        char *region_name, *region_caption;
        /* Client sets window title */
        qws_cmd_region_name_t *cmd =
            (qws_cmd_region_name_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        assert(win);

        if (qws_convert_to_narrow_unicode(&region_name, (const wchar_t *)
                incoming_pkt->raw_data, cmd->name_len * 2) < 0) {
            fprintf(stderr, "[qwswayland] Illegal region name from client %d\n",
                cl->client_id);
            free(region_name);
            break;
        }
        if (qws_convert_to_narrow_unicode(&region_caption, (const wchar_t *)
                &((char *) incoming_pkt->raw_data)[cmd->name_len * 2], cmd->caption_len * 2) < 0) {
            fprintf(stderr, "[qwswayland] Illegal region caption from client %d\n",
                cl->client_id);
            free(region_name);
            free(region_caption);
            break;
        }

        qwswl_set_window_name(win, region_name, region_caption);

        break;
    }

    case QWS_CMD_REPAINT_REGION: {
        /* Client has finished painting, requests compositor to show it */
        qws_cmd_repaint_region_t *cmd =
            (qws_cmd_repaint_region_t *)incoming_pkt->simple_data;
        qws_rect_t *rects = (qws_rect_t *)incoming_pkt->raw_data;

        qwswl_window_t *win = 
            qwswl_lookup_window_on_client(cl, cmd->window);
        if (!win->wl_surface) {
            qwswl_create_window(state, win, 
                qwswl_find_active_top_in_stack(cl), cmd->window_flags);
            qwswl_stack_dump(cl);
        }

        qwswl_update_surface(state, win, rects, cmd->nrectangles);
            //     const qws_rect_t window_rect =
            // { win->geometry.x, win->geometry.y, win->geometry.width, win->geometry.height };
            // qwswl_update_surface(state, win, &window_rect, 1);

        qws_lock_unlock(&cl->lock, QWS_LOCK_REGIONEVENT);
        
        break;
    }

    case QWS_CMD_REGION_MOVE: {
        qws_cmd_region_move_t *cmd =
            (qws_cmd_region_move_t *)incoming_pkt->simple_data;
        (void) cmd;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        assert(win);
        win->geometry.move_off_x += cmd->dx;
        win->geometry.move_off_y += cmd->dy;

        break;
    }
    case QWS_CMD_REGION_DESTROY: {
        qws_cmd_region_destroy_t *cmd =
            (qws_cmd_region_destroy_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        assert(win);
        
        qwswl_destroy_window(state, win);

        break;
    }

    case QWS_CMD_CHANGE_ALTITUDE: {
        qws_cmd_change_altitude_t *cmd =
            (qws_cmd_change_altitude_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        if (!win) {
            fprintf(stderr, "[qwswayland] Illegal window id %d from client %d\n",
                cmd->window, cl->fd);
            assert(false);
        }

        switch (cmd->altitude) {
            case QWS_ALTITUDE_STAYS_ON_TOP:
                qwswl_stack_move_to_top(cl, win);
                if (win->parent)
                    wl_subsurface_place_above(win->wl_subsurface,
                        win->parent->wl_surface);
                break;
            case QWS_ALTITUDE_RAISE:
                qwswl_stack_move_up(cl, win);
                if (win->parent)
                    wl_subsurface_place_above(win->wl_subsurface,
                        win->parent->wl_surface);
                break;
            case QWS_ALTITUDE_LOWER:
                qwswl_stack_move_down(cl, win);
                if (win->parent)
                    wl_subsurface_place_below(win->wl_subsurface,
                        win->parent->wl_surface);
                break;
        }

        const qws_rect_t window_rect =
            { win->geometry.x, win->geometry.y, win->geometry.width, win->geometry.height };
        if (win->wl_surface)
            qwswl_update_surface(state, win, &window_rect, 1);

        break;
    }

    case QWS_CMD_REQUEST_FOCUS: {
        // qws_cmd_request_focus_t *cmd =
        //     (qws_cmd_request_focus_t *)incoming_pkt->simple_data;

        // qws_packet_t *evt = qws_make_focus_event(cmd->window, cmd->flag);
        // assert(evt);

        // qws_trace_packet(cl->client_id, evt, true);
        // qws_write_packet(cl->fd, evt);
        // qws_packet_free(evt);

        break;
    }

    case QWS_CMD_SET_OPACITY: {
        qws_cmd_set_opacity_t *cmd =
            (qws_cmd_set_opacity_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_lookup_window_on_client(cl, cmd->window);
        assert(win);
        /* Wayland doesn't have per-surface opacity natively.
         * Could use wp_alpha_modifier or compositor-specific protocol. */
        break;
    }

    case QWS_CMD_GRAB_MOUSE: {
        /* Acknowledge but don't actually do anything special */
        break;
    }

    case QWS_CMD_GRAB_KEYBOARD: {
        break;
    }

    case QWS_CMD_ADD_PROPERTY:
    case QWS_CMD_SET_PROPERTY:
    case QWS_CMD_REMOVE_PROPERTY:
    case QWS_CMD_GET_PROPERTY: {
        /* Minimal property store - most QWS apps don't rely heavily on this.
         * TODO: implement a simple key-value store if needed. */
        if (type == QWS_CMD_GET_PROPERTY) {
            qws_cmd_get_property_t *cmd =
            (qws_cmd_get_property_t *)incoming_pkt->simple_data;
            /* Send empty reply */
            qws_packet_t *reply = qws_make_property_reply(
                cmd->window, cmd->property, NULL, 0);
            qws_trace_packet(cl->client_id, reply, true);
            qws_write_packet(cl->fd, reply);
        }
        break;
    }

    case QWS_CMD_DEFINE_CURSOR:
    case QWS_CMD_SELECT_CURSOR:
    case QWS_CMD_POSITION_CURSOR: {
        /* TODO: translate to Wayland cursor via wl_cursor_theme */
        break;
    }

    case QWS_CMD_QCOP_REGISTER:
    case QWS_CMD_QCOP_SEND: {
        /* QCop IPC: could be bridged to D-Bus or handled internally.
         * For now, silently ignore. */
        break;
    }

    case QWS_CMD_IM_UPDATE: {
        /* A widget is reporting an IM focus or context change.
         * FocusIn/FocusOut tell us which widget_id holds the IM cursor;
         * Update signals that the cursor position or selection changed.
         * We don't forward this to the Wayland compositor — Wayland IM
         * (text-input-v3 / input-method-v2) is managed separately.
         * No response is expected by the client for this command. */
        break;
    }

    case QWS_CMD_FONT: {
        /* Font usage notifications - not needed for our proxy */
        break;
    }

    case QWS_CMD_SHUTDOWN: {
        fprintf(stderr, "[qwswayland] Client %d requested shutdown\n",
                cl->client_id);
        qwswl_disconnect_client(state, cl);
        break;
    }

    default:
        fprintf(stderr, "[qwswayland] Unhandled command 0x%x from client %d\n",
                type, cl->client_id);
        // assert(false);
        break;
    }

    if (qws_is_synchronous_commmand(type)) {
        /* Increment communication semaphore after completing synchronous commands.
         * The client blocks in sendSynchronousCommand() and won't process the event 
         * until it can decrement the semaphore. The server has to increment the semaphore
         * first after sending the event. */
        qws_lock_unlock(&cl->lock, QWS_LOCK_COMMUNICATION);
    }
}