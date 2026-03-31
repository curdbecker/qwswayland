/*
 * window.h - QWSWayland proxy window and pixel buffer management
 * SPDX-License-Identifier: MIT
 */

#ifndef WINDOW_H
#define WINDOW_H

#include "lifecycle.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------
 * Per-window state: maps QWS window ID → Wayland surface
 * ----------------------------------------------------------- */

typedef struct qwswl_client qwswl_client_t;
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
        struct wl_shm_pool *pool;
        void             *pixels;       /* mmap of the anonymous fd */
        int               fd;
        size_t            size;
        int32_t           format;       /* wl_shm_format */
        size_t            width;
        size_t            height;
    } server_shm;

    /* QWS client pixel buffer (SysV shm, permanently attached, not owned) */
    struct {
        qws_shm_t shm;                  /* shm_id, base (pixels) */
        int32_t   format;               /* QWS pixel format from the client */
    } client_shm;

    /* Per-surface opacity (wp_alpha_modifier, optional — NULL if unsupported) */
    struct wp_alpha_modifier_surface_v1 *alpha_modifier_surface;

    /* Window properties */
    char                 *name;
    char                 *caption;
    uint8_t              opacity;
    qws_window_flags_t   win_flags;
    bool                 fixed;
} qwswl_window_t;

/* Allocate data strctures to collect information about a QWS window.*/
qwswl_window_t *qwswl_allocate_window(qwswl_client_t *client);
/* Create a Wayland-backed QWS window.*/
void qwswl_create_window(qwswl_state_t *state, qwswl_window_t *win, qwswl_window_t *parent,
    qws_window_flags_t window_flags);

/* Destroy a window and release all associated Wayland and shm resources. */
void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win);

/* Detach the client shm and hide the window by committing a NULL buffer. */
void qwswl_hide_window(qwswl_state_t *state, qwswl_window_t *win);

/* Set the xdg_toplevel title from the QWS region name/caption. */
void qwswl_set_window_name(qwswl_window_t *win, char *name, char *caption);

/* Apply a QWS opacity value (0–255) to the surface via wp_alpha_modifier.
 * Creates the alpha_modifier_surface on first call; emits a warning and
 * returns silently if the compositor does not support the protocol. */
void qwswl_set_opacity(qwswl_state_t *state, qwswl_window_t *win, uint8_t opacity);

/* Attach, reattach or detach the QWS client's SysV shm as a permanently-mapped
 * read-only buffer on the window. Detaches any previously attached mapping
 * when the shm_id changes. */
void qwswl_attach_client_shm(qwswl_window_t *win, int shm_id,
                              int32_t width, int32_t height);
void qwswl_detach_client_shm(qwswl_window_t *win);


/* Copy pixels from QWS client's shared memory into the Wayland buffer,
 * and commit the surface. */
void qwswl_update_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects);

/* Update a window's geometry if necessary. */
void qwswl_update_geometry(qwswl_state_t *state, qwswl_window_t *win,
                                qws_rect_t *rects, int32_t nrects);

/* Translate a window by (dx, dy) including the window's stored 
 * rects. Returns false if the window cannot be moved as requested
 * e.g. if it has no valid region yet. The window state will then 
 * be not modified. */
bool qwswl_move_window(qwswl_state_t *state, qwswl_window_t *win,
                       int32_t dx, int32_t dy);

/* -----------------------------------------------------------
 * Hash-table + lookup helpers
 * ----------------------------------------------------------- */

/* Resolve a Wayland surface to its in-use QWS window. */
qwswl_window_t *qwswl_surface_to_win(struct wl_surface *surface);

#ifdef __cplusplus
}
#endif

#endif /* WINDOW_H */
