
/*
 * lifecycle.c - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#include "lifecycle.h"
#include "seat.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>

#include "xdg-output-unstable-v1-client-protocol.h"

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
                qws_ipc_type_t ipc_type)
{
    memset(state, 0, sizeof(*state));
    state->ipc_type = ipc_type;
    state->qws_server_fd = -1;
    state->epoll_fd = -1;
    state->screen_width = width;
    state->screen_height = height;
    state->screen_depth = depth;
    state->running = true;

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

    /* ---- Compute socket path once ---- */
    if (qws_socket_path(qws_display, state->socket_path,
                         sizeof(state->socket_path)) != 0) {
        fprintf(stderr, "[qwswayland] Failed to compute socket path\n");
        return -1;
    }

    /* ---- Create QWS server socket ---- */
    state->qws_server_fd = qws_server_listen(state->socket_path);
    if (state->qws_server_fd < 0) {
        fprintf(stderr, "[qwswayland] Failed to create QWS socket at %s: %s\n",
                state->socket_path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[qwswayland] QWS server listening on %s\n",
            state->socket_path);

    /* Create a shared memory region from the server-side to share display information
     * with the client. This looks to be a legacy way from earlier Qt Versions to share
     * information between client and server. By now, the only real use that is left
     * is apparently the sharing of override cursors - which we likely won't do.
     * However, the client refuses to start without it, so we unfortunately need 
     * to create it.*/
    if (qws_shm_create(&state->display_shm, QWSWL_SHM_SIZE, state->ipc_type) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create display shm\n");
        return -1;
    }

    /* ---- Set up epoll ---- */
    state->epoll_fd = epoll_create1(0);
    if (state->epoll_fd < 0) {
        perror("[qwswayland] epoll_create1");
        return -1;
    }

    fprintf(stderr, "[qwswayland] Initialized: %dbpp\n", depth);
    return 0;
}

/* ================================================================
 * Shutdown
 * ================================================================ */

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

    // /* Disconnect all clients */
    // for (int i = 0; i < QWSWL_MAX_CLIENTS; i++) {
    //     qwswl_client_t *cl = &state->clients[i];
    //     if (cl->in_use)
    //         qwswl_disconnect_client(state, cl);
    // }

    /* Clean up QWS */
    if (state->qws_server_fd >= 0) {
        close(state->qws_server_fd);
        unlink(state->socket_path);
    }

    qws_shm_destroy(&state->display_shm);

    if (state->epoll_fd >= 0)
        close(state->epoll_fd);

    state->running = false;
}

