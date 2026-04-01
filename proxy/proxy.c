/*
 * qwswayland.c - QWSWayland proxy daemon implementation
 * SPDX-License-Identifier: MIT
 */

#include "proxy.h"
#include "client.h"
#include "window.h"
#include "qws_event_factory.h"
#include "qws_unicode.h"
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

static void create_ids(qwswl_client_t *cl, int32_t count)
{
    int32_t first_id = qwswl_allocate_ids(cl, count);

    qws_packet_t *evt = qws_make_creation_event(first_id, count);
    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
}

static void send_region_event(qwswl_client_t *cl, qwswl_window_t *win,
                               qws_rect_t *rects, int32_t nrects)
{
    qws_packet_t *evt = qws_make_region_event(win->qws_id, 0, rects, nrects);
    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
    qwslock_unlock(cl->lock, QWS_LOCK_REGIONEVENT);
}

/*
 * This is a rather convenient hack to force the QWS client to request
 * a window repaint from us.
 *
 * Like for a region command, a region event without rects is 
 * interpreted as the surface currently not being visible anymore, e.g.
 * due to being obscured by another window.
 *
 * If we then immediately follow this by a region event with the
 * expected rects of a window, the client then assumes that the server
 * might does not have an up-to-date surface content of the window 
 * anymore and therefore requests an explicit repaint of the window 
 * immediately.
 */
__attribute__((unused))
static void force_window_repaint(qwswl_window_t *win)
{
    assert(win);
    qwswl_client_t *cl = win->client;
    send_region_event(cl, win, NULL, 0);
    send_region_event(cl, win, win->geometry.rects, win->geometry.nrects);
};

static void send_max_region_event(qwswl_state_t *state, qwswl_client_t *cl)
{
    /* Report a somewhat smaller screen size, so that a window does not
     * try to be larger than the actually available screen size which
     * might contain a task bar or other shell elements.
     * 
     * And apparently the window id does not matter at all.... It is
     * at all only meant to find the correct screen, but well I doubt
     * we ever have more than one screen that Qt should know about, so
     * any id is apparently fine. */
    qws_packet_t *evt = qws_make_max_window_rect_event(
        1, 0, 0, state->screen_width - 100, state->screen_height - 100
    );
    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
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
            cl->lock = qwslock_open(lock_id);
            if (cl->lock) {
                fprintf(stderr, "[qwswayland] Client %d: attached to lock id=%d\n",
                        cl->client_id, lock_id);
            } else {
                fprintf(stderr, "[qwswayland] Client %d: FAILED to attach lock id=%d\n",
                        cl->client_id, lock_id);
            }
        } else {
            fprintf(stderr, "[qwswayland] Client %d: error no lock id in Identify\n", cl->client_id);
        }

        /* Send Connected event */
        char display_spec[128];
        snprintf(display_spec, sizeof(display_spec), "vnc:size=%dx%d:depth=%d:%d", 
            state->screen_width, state->screen_height, state->screen_depth, state->qws_display);
        qws_packet_t *conn = qws_make_connected_event(
            cl->client_id, state->display_shm.shm_id, display_spec);
        qws_trace_packet(cl->client_id, conn, true);
        qws_write_packet(cl->fd, conn);

        /*
        * The client blocks in waitForCreation() after connecting and
        * will not proceed until it receives a Creation event. The server
        * must proactively provide IDs — the client does not send a
        * Create command first. In general, there seem to be 30 resources
        * to be created by default - no idea why. */
        create_ids(cl, 30);

        /* Send a single region event that will be valid for all windows
         * on our only screen. */
        send_max_region_event(state, cl);

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

        create_ids(cl, cmd->count);
        break;
    }

    case QWS_CMD_REGION: {
        /* Client requests a screen region for its window */
        qws_cmd_region_request_t *cmd =
            (qws_cmd_region_request_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);

        /* Extract raw data */
        int32_t nrects = cmd->nrectangles;
        qws_rect_t *rects = (qws_rect_t *)incoming_pkt->raw_data;
        char * surface_key;
        uint8_t *surface_data = 
            &((uint8_t *) incoming_pkt->raw_data)[
                cmd->nrectangles * sizeof(qws_rect_t) + cmd->surfacekeylength * 2
            ];

        if (nrects > 0) {
            if (qws_convert_from_utf16(&surface_key, (const uint8_t *)
                    &((char *) incoming_pkt->raw_data)[cmd->nrectangles * sizeof(qws_rect_t)],
                    cmd->surfacekeylength, QWS_UTF16_LE, NULL) != 0) {
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
        
        qwslock_unlock(cl->lock, QWS_LOCK_REGIONEVENT);

        break;
    }

    case QWS_CMD_REGION_NAME: {
        char *region_name, *region_caption;
        /* Client sets window title */
        qws_cmd_region_name_t *cmd =
            (qws_cmd_region_name_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);

        if (qws_convert_from_utf16(&region_name, (const uint8_t *)
                incoming_pkt->raw_data, cmd->name_bytes / 2, QWS_UTF16_LE, NULL) != 0) {
            fprintf(stderr, "[qwswayland] Illegal region name from client %d\n",
                cl->client_id);
            free(region_name);
            break;
        }
        if (qws_convert_from_utf16(&region_caption, (const uint8_t *)
                &((char *) incoming_pkt->raw_data)[cmd->name_bytes],
                cmd->caption_bytes / 2, QWS_UTF16_LE, NULL) != 0) {
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
            qwswl_find_or_allocate_window(cl, cmd->window);
        if (!win->wl_surface) {
            qwswl_create_window(state, win, 
                qwswl_find_active_top_in_stack(cl), cmd->window_flags);
            qwswl_stack_dump(cl);
        }

        qwswl_update_surface(state, win, rects, cmd->nrectangles);
        /* This might be not exactly what we want, since this will 
         * affect the entire surface and not just the ones that
         * have been specified by the client... Who knows? */
        qwswl_set_opacity(state, win, cmd->opaque ? 255 : 0);
        
        break;
    }

    case QWS_CMD_REGION_MOVE: {
        qws_cmd_region_move_t *cmd =
            (qws_cmd_region_move_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);

        /*
         * The QWS protocol seems to be filled with quite a lot of surprises.
         *
         * The RegionMove command physically moves the window on the screen and
         * thereby also translates the rects reported by the client to the
         * server as expected by the move, e.g. when a rect is moved by (dx,dy),
         * then all coordinates in each rect are also moved by (dx,dy).
         *
         * Since we need to map the Wayland-local pointer coordinates to QWS 
         * global coordinates, we are even more required to have a shared
         * understanding about the window geometry with the client. Therefore,
         * it is rather shocking to all involved parties when the coordinate
         * system does not seem to agree anymore, since the cursor position
         * is suddenly replaced with the one obtained from shared memory that
         * is zero-initialized by default. This basically causes us to add
         * a large offset to the position as these are basically then 
         * also window-local coordinates... which will make the pointer jump
         * a lot.
         *
         * If a pointer should jump while a move operation (using the window
         * context menu etc.) is still happening, then client will think we
         * moved our window quite a lot... and this delta will get even
         * larger with every subsequent move operation.
         */
        if (qwswl_move_window(state, win, cmd->dx, cmd->dy)) {
            /* Send region ack with the translated (clipped) rects */
            qws_packet_t *evt = qws_make_region_event(
                cmd->window, 0, win->geometry.rects, win->geometry.nrects);
            qws_trace_packet(cl->client_id, evt, true);
            qws_write_packet(cl->fd, evt);
            qws_packet_free(evt);

            qwslock_unlock(cl->lock, QWS_LOCK_REGIONEVENT);
        }

        break;
    }
    case QWS_CMD_REGION_DESTROY: {
        qws_cmd_region_destroy_t *cmd =
            (qws_cmd_region_destroy_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);
        
        qwswl_destroy_window(state, win);

        break;
    }

    case QWS_CMD_CHANGE_ALTITUDE: {
        qws_cmd_change_altitude_t *cmd =
            (qws_cmd_change_altitude_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);

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

        break;
    }

    case QWS_CMD_REQUEST_FOCUS: {
        /* 
         * There is not that much that we can do for the client here, since the 
         * compositor controls basically all user-related input in an (sometimes 
         * well-meaning, but a bit misguided IMHO) effort to protect the user
         * from weird or intrusive behaviour by applications. 
         * 
         * For now we assume that a command without a related focus event will
         * be considered by the client as a decline of the request.
         * In doubt, we might need to force focus events for the currently
         * focused seat device again to make sure that there is the same shared
         * understanding?
         */
        break;
    }

    case QWS_CMD_SET_OPACITY: {
        qws_cmd_set_opacity_t *cmd =
            (qws_cmd_set_opacity_t *)incoming_pkt->simple_data;

        qwswl_window_t *win = qwswl_find_or_allocate_window(cl, cmd->window);
        qwswl_set_opacity(state, win, cmd->opacity);
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
    
    if (qws_is_synchronous_command(type)) {
        /* Increment communication semaphore after completing synchronous commands.
         * The client blocks in sendSynchronousCommand() and won't process the event 
         * until it can decrement the semaphore. The server has to increment the semaphore
         * first after sending the event. */
        qwslock_unlock(cl->lock, QWS_LOCK_COMMUNICATION);
    }
}