/*
 * lifecycle.h - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include "clipboard.h"
#include "cursor.h"
#include "property_store.h"
#include "seat.h"

#include "qws_lock.h"
#include "qws_proto.h"

#include <limits.h>
#include <stdbool.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#ifdef HAVE_ALPHA_MODIFIER_V1
#include "alpha-modifier-v1-client-protocol.h"
#endif
#include "qt-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "stc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------
 * Screen driver selection
 * ----------------------------------------------------------- */

typedef enum {
    QWSWL_SCREEN_DRIVER_LINUXFB,
    QWSWL_SCREEN_DRIVER_VNC,
} qwswl_screen_driver_type_t;

typedef struct {
    char fb_device[32]; /* e.g. /dev/fb0 */
} qwswl_linuxfb_opts_t;

typedef struct {
    qwswl_screen_driver_type_t type;
    int32_t width;
    int32_t height;
    int32_t depth;
    bool use_interposer;
    union {
        qwswl_linuxfb_opts_t linuxfb;
    } opts;
} qwswl_screen_driver_opts_t;

typedef struct qwswl_client qwswl_client_t;
declare_hashmap(qwswl_client_map_t, int32_t, qwswl_client_t *);

/* Forward declaration; qwswl_window_t is defined in window.h */
typedef struct qwswl_window qwswl_window_t;

/* -----------------------------------------------------------
 * Main proxy state
 * ----------------------------------------------------------- */

typedef struct qwswl_state {
    /* QWS server side */
    int qws_server_fd;
    qws_display_paths_t display_paths;
    /* QWS display specification */
    char display_spec[128];

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
    uint32_t wl_seat_name; /* registry name for dynamic add/remove matching */
    struct wl_pointer *wl_pointer;
    struct wl_keyboard *wl_keyboard;
    struct wl_output *wl_output;

    /* xdg_wm_base for xdg shell (window management) */
    struct xdg_wm_base *xdg_wm_base;

    /* wl_subcompositor for child/subsurface windows */
    struct wl_subcompositor *wl_subcompositor;

#ifdef HAVE_ALPHA_MODIFIER_V1
    /* wp_alpha_modifier for per-surface opacity (optional, may be NULL) */
    struct wp_alpha_modifier_v1 *wp_alpha_modifier;
#endif

    /* zqt_shell_v1 for Qt-specific window management (optional, may be NULL) */
    struct zqt_shell_v1 *zqt_shell;

    struct xkb_context *xkb_context;

    qwswl_pointer_state_t pointer_state;
    qwswl_keyboard_state_t kbd_state;
    /* Most recent serial from any input event — required for clipboard
     * set_selection */
    uint32_t last_input_serial;

    /* Currently focused window. Single-screen assumption: with multiple screens
     * each screen would need its own slot. */
    qwswl_window_t *focused_window;

    /* Cursor state (wp_cursor_shape_v1 + bitmap fallback surface). */
    qwswl_cursor_t cursor;

    /* Wayland clipboard bridge */
    qwswl_clipboard_t clipboard;

    /* Global QWS property store (shared across all clients) */
    qwswl_prop_store_t prop_store;

    /* Event loop */
    bool running;
    int qws_epoll_fd;
    int loop_epoll_fd;

    /* Debug: draw a red border around each repaint rect */
    bool debug_draw_rects;
} qwswl_state_t;

/* Initialize the proxy: connect to Wayland, set up QWS server socket,
 * and initialize epoll. */
int qwswl_init(qwswl_state_t *state, int qws_display, bool debug_draw_rects,
               bool skip_fontdb_init,
               const qwswl_screen_driver_opts_t *screen_driver);

/* Clean shutdown: destroy all Wayland objects, disconnect clients,
 * and remove the QWS server socket. */
void qwswl_shutdown(qwswl_state_t *state);

/* Run the main epoll event loop (blocks until shutdown). */
int qwswl_run(qwswl_state_t *state);

/* Callback for qwswl_client_foreach; userdata is caller-defined. */
typedef void (*qwswl_client_cb_t)(qwswl_client_t *cl, void *userdata);

/* Call cb(cl, userdata) for every connected client. */
void qwswl_client_foreach(qwswl_state_t *state, qwswl_client_cb_t cb,
                          void *userdata);

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl);

static inline bool compositor_is_likely_qt(qwswl_state_t *state) {
    /* There might be an intended version to get the compositor name and
     * version, but for now that's more than enough for us. */
    return state->zqt_shell != NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* LIFECYCLE_H */
