/*
 * lifecycle.h - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include "qws_proto.h"

#include <stdbool.h>
#include <limits.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "stc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qwswl_client qwswl_client_t;
declare_hashmap(qwswl_client_map_t, int32_t, qwswl_client_t *);

/* -----------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------- */

#define QWSWL_SHM_SIZE       1024

/* -----------------------------------------------------------
 * Main proxy state
 * ----------------------------------------------------------- */

typedef struct qwswl_state {
    /* QWS server side */
    int                  qws_server_fd;
    char                 socket_path[PATH_MAX];

    qwswl_client_map_t   client_map;

    /* Shared memory region (display properties) */
    qws_shm_t            display_shm;

    /* IPC backend: QWS_IPC_SYSV or QWS_IPC_POSIX */
    qws_ipc_type_t       ipc_type;

    /* Screen parameters (reported to QWS clients) */
    int32_t              screen_width;
    int32_t              screen_height;
    int32_t              screen_depth;

    /* Wayland client side */
    struct wl_display    *wl_display;
    struct wl_registry   *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_shm        *wl_shm;
    struct wl_seat       *wl_seat;
    struct wl_pointer    *wl_pointer;
    struct wl_keyboard   *wl_keyboard;
    struct wl_output     *wl_output;

    /* xdg_wm_base for xdg shell (window management) */
    struct xdg_wm_base      *xdg_wm_base;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct zxdg_output_v1   *xdg_output;

    /* wl_subcompositor for child/subsurface windows */
    struct wl_subcompositor *wl_subcompositor;

    struct xkb_context  *xkb_context;

    /* Event loop */
    bool                 running;
    int                  qws_epoll_fd;
    int                  loop_epoll_fd;
} qwswl_state_t;

/* Initialize the proxy: connect to Wayland, set up QWS server socket,
 * and initialize epoll. */
int  qwswl_init(qwswl_state_t *state, int qws_display,
                int32_t width, int32_t height, int32_t depth,
                qws_ipc_type_t ipc_type);

/* Clean shutdown: destroy all Wayland objects, disconnect clients,
 * and remove the QWS server socket. */
void qwswl_shutdown(qwswl_state_t *state);

/* Run the main epoll event loop (blocks until shutdown). */
int  qwswl_run(qwswl_state_t *state);

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl);

#ifdef __cplusplus
}
#endif

#endif /* LIFECYCLE_H */
