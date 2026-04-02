/*
 * lifecycle.h - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include "seat.h"

#include "qws_lock.h"
#include "qws_proto.h"

#include <limits.h>
#include <stdbool.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "stc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qwswl_client qwswl_client_t;
declare_hashmap(qwswl_client_map_t, int32_t, qwswl_client_t *);

/* -----------------------------------------------------------
 * Main proxy state
 * ----------------------------------------------------------- */

typedef struct qwswl_state {
    /* QWS server side */
    int qws_server_fd;
    qws_display_paths_t display_paths;

    qwswl_client_map_t client_map;

    /* Shared memory region, lock and weirdly shared pointer
     * positions. */
    qws_shm_t display_shm;
    qlock_t *display_lock;
    int32_t *qt_last_x;
    int32_t *qt_last_y;

    /* Screen parameters (reported to QWS clients) */
    int32_t screen_width;
    int32_t screen_height;
    int32_t screen_depth;
    int qws_display;

    /* Wayland client side */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_shm *wl_shm;
    struct wl_seat *wl_seat;
    struct wl_pointer *wl_pointer;
    struct wl_keyboard *wl_keyboard;
    struct wl_output *wl_output;

    /* xdg_wm_base for xdg shell (window management) */
    struct xdg_wm_base *xdg_wm_base;

    /* wl_subcompositor for child/subsurface windows */
    struct wl_subcompositor *wl_subcompositor;

    /* wp_alpha_modifier for per-surface opacity (optional, may be NULL) */
    struct wp_alpha_modifier_v1 *wp_alpha_modifier;

    struct xkb_context *xkb_context;

    qwswl_pointer_state_t pointer_state;
    qwswl_keyboard_state_t kbd_state;

    /* Event loop */
    bool running;
    int qws_epoll_fd;
    int loop_epoll_fd;

    /* Debug: draw a red border around each repaint rect */
    bool debug_draw_rects;
} qwswl_state_t;

/* Initialize the proxy: connect to Wayland, set up QWS server socket,
 * and initialize epoll. */
int qwswl_init(qwswl_state_t *state, int qws_display, int32_t width,
               int32_t height, int32_t depth, bool debug_draw_rects);

/* Clean shutdown: destroy all Wayland objects, disconnect clients,
 * and remove the QWS server socket. */
void qwswl_shutdown(qwswl_state_t *state);

/* Run the main epoll event loop (blocks until shutdown). */
int qwswl_run(qwswl_state_t *state);

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl);

#ifdef __cplusplus
}
#endif

#endif /* LIFECYCLE_H */
