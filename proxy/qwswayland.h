/*
 * qwswayland.h - QWSWayland proxy daemon
 *
 * Acts as a QWS server to legacy Qt 4.8 apps while proxying their
 * window operations to a Wayland compositor via libwayland-client.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWSWAYLAND_H
#define QWSWAYLAND_H

#include "qws_proto.h"
#include "qws_lock.h"

#include <stdbool.h>
#include <limits.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------- */

#define QWSWL_MAX_CLIENTS    32
#define QWSWL_MAX_WINDOWS    256
#define QWSWL_SHM_SIZE       1024

/* -----------------------------------------------------------
 * Per-client state: one per connected QWS application
 * ----------------------------------------------------------- */

typedef struct qwswl_client {
    int              fd;            /* unix socket fd to QWS client */
    int32_t          client_id;
    qws_reader_t     reader;        /* incremental packet parser */
    bool             in_use;

    /* ID allocation: the server pre-allocates IDs for the client */
    int32_t          next_id;

    /* Per-client lock (3 semaphores: BackingStore, Communication, RegionEvent) */
    qws_lock_t       lock;
} qwswl_client_t;

/* -----------------------------------------------------------
 * Per-window state: maps QWS window ID → Wayland surface
 * ----------------------------------------------------------- */

typedef struct qwswl_window qwswl_window_t;
typedef struct qwswl_window {
    int32_t              qws_id;        /* QWS window id */
    qwswl_client_t       *client;

    /* Wayland objects */
    struct wl_surface    *wl_surface;
    /* Toplevel windows use xdg_surface + xdg_toplevel.
     * Child windows use wl_subsurface instead — xdg_surface/toplevel are NULL. */
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *xdg_toplevel;
    struct wl_subsurface *wl_subsurface;

    qwswl_window_t       *parent;

    /* Geometry as known by the QWS client */
    qwswl_geometry_t     geometry;

    /* Wayland-side pixel buffer: anonymous mmap fd + wl_buffer */
    struct {
        struct wl_buffer *buffer;
        void             *pixels;       /* mmap of the anonymous fd */
        int               fd;
        size_t            size;
        int32_t           format;       /* wl_shm_format */
    } server_shm;

    /* QWS client pixel buffer (SysV shm, permanently attached, not owned) */
    struct {
        qws_shm_t shm;                  /* shm_id, base (pixels) */
        int32_t   format;               /* QWS pixel format from the client */
        int32_t   width;                /* buffer dimensions as reported by the client */
        int32_t   height;
    } client_shm;

    /* Window properties */
    char                 name[256];
    char                 caption[256];
    int32_t              opacity;       /* 0-255 */
    bool                 visible;
    bool                 focused;

    bool                 always_on_top;

    qwswl_window_t       *upper_win;
    qwswl_window_t       *lower_win;

    bool                 allocated;
    bool                 in_use;
} qwswl_window_t;

/* -----------------------------------------------------------
 * Main proxy state
 * ----------------------------------------------------------- */

typedef struct qwswl_state {
    /* QWS server side */
    int                  qws_server_fd;
    char                 socket_path[PATH_MAX];  /* computed once at init */
    qwswl_client_t       clients[QWSWL_MAX_CLIENTS];
    qwswl_window_t       windows[QWSWL_MAX_WINDOWS];
    int32_t              next_window_id;

    qwswl_window_t       *top_window;

    /* Shared memory region (display properties) */
    qws_shm_t            display_shm;

    /* Display-level R/W lock (protects display_shm) */
    qws_lock_t           display_lock;

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

    /* Event loop control */
    bool                 running;
    int                  epoll_fd;
} qwswl_state_t;

/* -----------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------- */

/* Initialize the proxy: connect to Wayland, set up QWS server socket */
int  qwswl_init(qwswl_state_t *state, int qws_display,
                int32_t width, int32_t height, int32_t depth,
                qws_ipc_type_t ipc_type);

/* Run the main event loop (blocks until shutdown) */
int  qwswl_run(qwswl_state_t *state);

/* Clean shutdown */
void qwswl_shutdown(qwswl_state_t *state);

/* -----------------------------------------------------------
 * Client management
 * ----------------------------------------------------------- */

int  qwswl_accept_client(qwswl_state_t *state);
void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *client);
void qwswl_handle_client_data(qwswl_state_t *state, qwswl_client_t* client);

/* -----------------------------------------------------------
 * Command dispatch: process a QWS command from a client
 * ----------------------------------------------------------- */

void qwswl_dispatch_command(qwswl_state_t *state, qwswl_client_t *client,
                             qws_packet_t *pkt);

/* -----------------------------------------------------------
 * Window operations → Wayland
 * ----------------------------------------------------------- */

qwswl_window_t *qwswl_create_window(qwswl_state_t *state, qwswl_window_t *win,
    qwswl_client_t *client, bool is_first);
void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win);
void qwswl_update_window_region(qwswl_state_t *state, qwswl_window_t *win,
                                const qws_rect_t *rects, int32_t nrects,
                                int32_t width, int32_t height);
void qwswl_set_window_name(qwswl_state_t *state, qwswl_window_t *win,
                             const char *name, const char *caption);
void qwswl_focus_window(qwswl_state_t *state, qwswl_window_t *win);

/* -----------------------------------------------------------
 * Pixel buffer management
 * ----------------------------------------------------------- */

/* Attach (or reattach) the QWS client's SysV shm as a permanently-mapped
 * read-only buffer on the window.  Detaches any previously attached mapping
 * when the shm_id changes. */
void qwswl_attach_client_shm(qwswl_window_t *win, int shm_id,
                              int32_t width, int32_t height);

/* Create a wl_buffer backed by shared memory for a window's surface.
 * Called when the window geometry changes. */
int  qwswl_create_buffer(qwswl_state_t *state, qwswl_window_t *win,
                          int32_t width, int32_t height);

/* Copy pixels from QWS client's shared memory into the Wayland buffer
 * and commit the surface. */
void qwswl_commit_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects);

/* -----------------------------------------------------------
 * Lookup helpers
 * ----------------------------------------------------------- */

qwswl_window_t *qwswl_find_window(qwswl_state_t *state, int32_t qws_id, bool in_use);

#ifdef __cplusplus
}
#endif

#endif /* QWSWAYLAND_H */