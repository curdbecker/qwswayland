
/*
 * lifecycle.c - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#include "lifecycle.h"
#include "client.h"
#include "debug.h"
#include "proxy.h"
#include "seat.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>

#include "xdg-output-unstable-v1-client-protocol.h"

#define T qwswl_client_map_t, int32_t, qwswl_client_t*
#define i_declared
#include "stc/hashmap.h"

extern const struct wl_seat_listener seat_listener;

/* ================================================================
 * xdg_wm_base listener (keepalive ping/pong)
 * ================================================================ */

static void xdg_base_wm_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                               uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_base_wm_ping,
};

/* ================================================================
 * xdg_output listener
 * ================================================================ */

static void xdg_output_logical_position(void *data,
                                        struct zxdg_output_v1 *output,
                                        int32_t x, int32_t y)
{
    (void)data; (void)output;
    QWS_TRACE("xdg_output: logical_position %d,%d", x, y);
}

static void xdg_output_logical_size(void *data,
                                    struct zxdg_output_v1 *output,
                                    int32_t width, int32_t height)
{
    (void)output;
    qwswl_state_t *state = (qwswl_state_t *)data;

    QWS_TRACE("xdg_output: logical_size %dx%d", width, height);

    if (state->screen_width == 0 && state->screen_height == 0) {
        state->screen_width = width;
        state->screen_height = height;
    } else {
        QWS_TRACE("WARNING: screen geometry was forced via user input");
    }
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *output)
{
    (void)data; (void)output;
    QWS_TRACE("xdg_output: done");
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *output,
                             const char *name)
{
    (void)data; (void)output;
    QWS_TRACE("xdg_output: name \"%s\"", name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *output,
                                    const char *description)
{
    (void)data; (void)output;
    QWS_TRACE("xdg_output: description \"%s\"", description);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size     = xdg_output_logical_size,
    .done             = xdg_output_done,
    .name             = xdg_output_name,
    .description      = xdg_output_description,
};

/* ================================================================
 * Wayland registry listener
 * ================================================================ */

static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *interface,
                             uint32_t version)
{
    qwswl_state_t *state = (qwswl_state_t *)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        state->wl_compositor = wl_registry_bind(reg, name,
                                                 &wl_compositor_interface, version);
        QWS_TRACE("registry: found wl_compositor (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_shm") == 0) {
        state->wl_shm = wl_registry_bind(reg, name,
                                           &wl_shm_interface, 1);
        QWS_TRACE("registry: found wl_shm (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_seat") == 0) {
        state->wl_seat = wl_registry_bind(reg, name,
                                            &wl_seat_interface, version);
        QWS_TRACE("registry: found wl_seat (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_output") == 0) {
        state->wl_output = wl_registry_bind(reg, name,
                                              &wl_output_interface, version);
        QWS_TRACE("registry: found wl_output (name=%u, max_versionv=%u)", name, version);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        state->xdg_wm_base = wl_registry_bind(reg, name,
                                               &xdg_wm_base_interface, version);
        QWS_TRACE("registry: found xdg_wm_base (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "zxdg_output_manager_v1") == 0) {
        state->xdg_output_manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, version);
        QWS_TRACE("registry: found zxdg_output_manager_v1 (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        state->wl_subcompositor = wl_registry_bind(reg, name,
                                                    &wl_subcompositor_interface, version);
        QWS_TRACE("registry: found wl_subcompositor (name=%u, max_version=%u)", name, version);
    } else {
        QWS_TRACE("registry: skipped %s (name=%u, v=%u)", interface, name, version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                    uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ================================================================
 * Initialization
 * ================================================================ */

int qwswl_init(qwswl_state_t *state, int qws_display,
                int32_t width, int32_t height, int32_t depth,
                bool debug_draw_rects)
{
    memset(state, 0, sizeof(*state));
    state->qws_display = qws_display;
    state->debug_draw_rects = debug_draw_rects;
    state->qws_server_fd = -1;
    state->qws_epoll_fd = -1;
    state->screen_width = width;
    state->screen_height = height;
    state->screen_depth = depth;
    state->running = true;

    qwswl_client_map_t_init();

    state->xkb_context = xkb_context_new(0);
    if (!state->xkb_context) {
        fprintf(stderr, "[qwswayland] Failed to create xkb context\n");
        return -1;
    }

    /* ---- Connect to Wayland ---- */
    state->wl_display = wl_display_connect(NULL);
    if (!state->wl_display) {
        fprintf(stderr, "[qwswayland] Failed to connect to Wayland display\n");
        return -1;
    }

    state->wl_registry = wl_display_get_registry(state->wl_display);
    wl_registry_add_listener(state->wl_registry, &registry_listener, state);

    /* Round-trip to get globals */
    wl_display_roundtrip(state->wl_display);

    if (!state->wl_compositor) {
        fprintf(stderr, "[qwswayland] No wl_compositor found\n");
        return -1;
    }
    if (!state->wl_shm) {
        fprintf(stderr, "[qwswayland] No wl_shm found\n");
        return -1;
    }
    
    if (!state->wl_output) {
        fprintf(stderr, "[qwswayland] No wl_output found\n");
        return -1;
    }
    
    if (!state->xdg_output_manager) {
        fprintf(stderr, "[qwswayland] No xdg_output_manager found\n");
        return -1;
    }

    if (!state->xdg_wm_base) {
        fprintf(stderr, "[qwswayland] No xdg_wm_base found\n");
        return -1;
    }

    /* Add seat listener if we got a seat. 
     *
     * For some weird reason this needs to be registered first (at least before the 
     * xdg_output_manager). Otherwise, we will be able to register the listener, but will not 
     * get a wl_seat_capability callback anymore and therefore won't have any input. */
    if (state->wl_seat) {
        wl_seat_add_listener(state->wl_seat, &seat_listener, state);
        wl_display_roundtrip(state->wl_display);
    } else {
        fprintf(stderr, 
            "[qwswayland] WARNING: No wl_seat found - input devices will be unavailable\n" \
            "             THIS IS LIKELY NOT WHAT YOU WANT - EVEN WINDOW DRAWING COULD BE AFFECTED!\n");
    }

    state->xdg_output = 
        zxdg_output_manager_v1_get_xdg_output(state->xdg_output_manager, state->wl_output);
    if (!state->xdg_output) {
        fprintf(stderr, "[qwswayland] Failed to create xdg_output for wl_output\n");
        return -1;
    }
    zxdg_output_v1_add_listener(state->xdg_output, &xdg_output_listener, state);
    wl_display_roundtrip(state->wl_display);

    xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, NULL);
    wl_display_roundtrip(state->wl_display);

    /* ---- Initialise display directory and derive all paths ---- */
    if (qws_init_display_dir(qws_display, &state->display_paths) != 0) {
        fprintf(stderr, "[qwswayland] Failed to initialise display directory\n");
        return -1;
    }

    if (qws_build_font_database(state->display_paths.fontdb) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create font database\n");
        return -1;
    }

    /* ---- Create QWS server socket ---- */
    state->qws_server_fd = qws_server_listen(state->display_paths.socket);
    if (state->qws_server_fd < 0) {
        fprintf(stderr, "[qwswayland] Failed to create QWS socket at %s: %s\n",
                state->display_paths.socket, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[qwswayland] QWS server listening on %s\n",
            state->display_paths.socket);

    /* Create a shared memory region from the server-side to share display information
     * with the client... and don't forget about the lock.
     *
     * This looks like a legacy way in earlier Qt Versions for sharing information 
     * between client and server. By now, the only real use that is left
     * is apparently the sharing of override cursors - which we likely won't do.
     * 
     * However, the client refuses to start without it, so we unfortunately need 
     * the shm region and its lock.*/
    if (qws_shm_create(&state->display_shm, QWS_DISPLAY_SHM_SIZE) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create display shm\n");
        return -1;
    }
    state->display_lock = qlock_create(state->display_paths.socket, 'd');
    if (state->display_lock == NULL) {
        fprintf(stderr, "[qwswayland] Failed to create display lock\n");
        return -1;
    }

    /* ---- Set up epoll ---- */
    state->qws_epoll_fd = epoll_create1(0);
    if (state->qws_epoll_fd < 0) {
        perror("[qwswayland] epoll_create1 qws_epoll_fd");
        return -1;
    }

    state->loop_epoll_fd = epoll_create1(0);
    if (state->loop_epoll_fd < 0) {
        perror("[qwswayland] epoll_create1 loop_epoll_fd");
        return -1;
    }

    fprintf(stderr, "[qwswayland] Initialized: %dbpp\n", depth);
    qwswl_debug_init(state);
    return 0;
}

/* ================================================================
 * Shutdown
 * ================================================================ */

/* disconnects the client without updating the hashmap, so it is safe 
 * to iterate through it while disconnecting clients */
static void qwswl_disconnect_client_no_hashmap(qwswl_state_t *state, 
    qwswl_client_t *cl);

void qwswl_shutdown(qwswl_state_t *state)
{
    assert(state->running);

    fprintf(stderr, "[qwswayland] Shutting down\n");

    /* Clean up Wayland */
    if (state->wl_pointer)
        wl_pointer_destroy(state->wl_pointer);
    if (state->wl_keyboard) {
        qwswl_keyboard_data_t *kbd_data =
            wl_keyboard_get_user_data(state->wl_keyboard);
        if (kbd_data) {
            xkb_state_unref(kbd_data->xkb_state);
            xkb_keymap_unref(kbd_data->xkb_keymap);
            free(kbd_data);
        }
        wl_keyboard_destroy(state->wl_keyboard);
    }
    if (state->xkb_context)
        xkb_context_unref(state->xkb_context);
    if (state->wl_seat)
        wl_seat_destroy(state->wl_seat);
    if (state->xdg_wm_base)
        xdg_wm_base_destroy(state->xdg_wm_base);
    if (state->xdg_output_manager)
        zxdg_output_manager_v1_destroy(state->xdg_output_manager);
    if (state->wl_subcompositor)
        wl_subcompositor_destroy(state->wl_subcompositor);
    if (state->wl_shm)
        wl_shm_destroy(state->wl_shm);
    if (state->wl_compositor)
        wl_compositor_destroy(state->wl_compositor);
    if (state->wl_output)
        wl_output_destroy(state->wl_output);
    if (state->wl_registry)
        wl_registry_destroy(state->wl_registry);
    if (state->wl_display) {
        wl_display_flush(state->wl_display);
        wl_display_disconnect(state->wl_display);
    }

    /* Disconnect all clients */
    for(c_each_kv(client_id, cl, qwswl_client_map_t, state->client_map)) {
        qwswl_disconnect_client_no_hashmap(state, *cl);
    }

    /* Clean up QWS */
    if (state->qws_server_fd >= 0) {
        close(state->qws_server_fd);
        unlink(state->display_paths.socket);
    }

    qws_shm_destroy(&state->display_shm);

    if (state->display_lock != NULL)
        qlock_destroy(state->display_lock);

    if (state->qws_epoll_fd >= 0)
        close(state->qws_epoll_fd);
    
    if (state->loop_epoll_fd >= 0)
        close(state->loop_epoll_fd);

    state->running = false;
}

/* ================================================================
 * Main event loop and helpers
 * ================================================================ */

static qwswl_client_t *qwswl_accept_client(qwswl_state_t *state)
{
    static int32_t next_client_id = 1;
    int fd = qws_server_accept(state->qws_server_fd);
    if (fd < 0)
        return NULL;

    qwswl_client_t *cl = qwswl_create_client(fd, next_client_id);
    assert(cl);

    /* Add to epoll */
    struct epoll_event ev = { .events = EPOLLIN,
                               .data.ptr = (void *)cl };
    epoll_ctl(state->qws_epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    qwswl_client_map_t_result res = 
        qwswl_client_map_t_insert(&state->client_map, next_client_id, cl);
    assert(res.inserted);

    next_client_id++;

    fprintf(stderr, "[qwswayland] Client connected (fd=%d)\n", fd);
    return cl;
}

static void qwswl_disconnect_client_no_hashmap(qwswl_state_t *state, qwswl_client_t *cl)
{
    epoll_ctl(state->qws_epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);

    qwswl_destroy_client(state, cl);
}

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl) {
    fprintf(stderr, "[qwswayland] Client %d disconnected\n",
        cl->client_id);
    
    assert(qwswl_client_map_t_erase(&state->client_map, cl->client_id));

    qwswl_disconnect_client_no_hashmap(state, cl);
}

int qwswl_run(qwswl_state_t *state)
{
    struct epoll_event loop_events[3];
    struct epoll_event qws_events[32];
    int wl_fd;
    
    wl_fd = wl_display_get_fd(state->wl_display);
    if (wl_fd < 0) {
        perror("[qwswayland] wl_display_get_fd");
        return 1;
    }
        
    /* Watch QWS server socket for new connections */
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.fd = state->qws_server_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, 
            state->qws_server_fd, &ev);
    }

    /* Watch Wayland display fd for events */
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.fd = wl_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev);
    }

    /* Watch qws client epoll fd for events */ {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.fd = state->qws_epoll_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, 
            state->qws_epoll_fd, &ev);
    }

    fprintf(stderr, "[qwswayland] Entering main loop\n");

    /* We're using two separate epoll fds in order to comply with wayland's
     * suggested event handling procedure while still being able
     * to use wl_display_roundtrip in our own event handling code to simplify
     * pointer/key events without having to resort to stray pointers (maybe?).
     * 
     * The main issue is that wl_display_roundtrip would block if the thread
     * is preparing to read, since the queue is then actually locked for
     * reading, so we would be essentially creating a deadlock. However, this
     * still would be very convenient e.g. when handling window/surface desctruction.
     * 
     * Therefore, we're handling first reading from the wayland event queue
     * to fetch all pending events or we are cancelling the read if there are 
     * no events to process. Afterwards, we're separately processing all events
     * that happened on the qws client sockets. 
     * 
     * The neat thing here is that an epoll fd is actually epoll-able itself, 
     * so we do not have to poll separately on the chance the are QWS events, 
     * but only if we are know that there must be events from our first epoll.
     * */

    while (true) {
        /* Flush Wayland before blocking */
        while (wl_display_prepare_read(state->wl_display) != 0)
            wl_display_dispatch_pending(state->wl_display);
        wl_display_flush(state->wl_display);

        bool had_wayland = false;
        bool had_qws = false;

        {
            int nfds = epoll_wait(state->loop_epoll_fd, loop_events, 3, 100);
            if (nfds < 0 && errno != EINTR) {
                if (errno == EBADFD)
                    /* we're being shut down */
                    return 0;

                perror("[qwswayland] epoll_wait loop_epoll_fd");
                return 1;
            }

            for (int i = 0; i < nfds; i++) {
                if (loop_events[i].data.fd == state->qws_server_fd) {
                    /* New QWS client connection */
                    assert(qwswl_accept_client(state));
                } else if (loop_events[i].data.fd == wl_fd) {
                    /* Wayland events */
                    had_wayland = true;
                } else if (loop_events[i].data.fd == state->qws_epoll_fd) {
                    /* QWS events */
                    had_qws = true;
                }
            }
        }

        if (had_wayland) {
            wl_display_read_events(state->wl_display);
        } else {
            wl_display_cancel_read(state->wl_display);
        }

        if (had_qws) {
            int nfds = epoll_wait(state->qws_epoll_fd, qws_events, 32, 100);
            if (nfds < 0) {
                perror("[qwswayland] epoll_wait ");
                return 1;
            }

            for (int i = 0; i < nfds; i++) {
                qwswl_handle_client_data(state, 
                    (qwswl_client_t *) qws_events[i].data.ptr);
            }
        }

        wl_display_flush(state->wl_display);
    }

    return 0;
}