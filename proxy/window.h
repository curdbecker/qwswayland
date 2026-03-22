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

#define QWSWL_MAX_WINDOWS    256

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
    char                 *name;
    char                 *caption;
    
     bool                 always_on_top;
} qwswl_window_t;

/* Create a new Wayland-backed QWS window.
 * If win is non-NULL, the existing allocated slot is initialized in place.
 * is_first must be true for the first (toplevel) window of a client. */
qwswl_window_t *qwswl_create_window(qwswl_state_t *state, qwswl_client_t *client, 
    bool is_toplevel);

/* Destroy a window and release all associated Wayland and shm resources. */
void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win);

/* Set the xdg_toplevel title from the QWS region name/caption. */
void qwswl_set_window_name(qwswl_window_t *win, char *name, char *caption);

/* Attach (or reattach) the QWS client's SysV shm as a permanently-mapped
 * read-only buffer on the window. Detaches any previously attached mapping
 * when the shm_id changes. */
void qwswl_attach_client_shm(qwswl_window_t *win, int shm_id,
                              int32_t width, int32_t height);

/* Copy pixels from QWS client's shared memory into the Wayland buffer,
 * and commit the surface. */
void qwswl_update_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects);

/* Update a window's geometry if necessary. */
void qwswl_update_geometry(qwswl_state_t *state, qwswl_window_t *win,
                                const qws_rect_t *rects, int32_t nrects);

/* -----------------------------------------------------------
 * Lookup helpers
 * ----------------------------------------------------------- */

/* Resolve a Wayland surface to its in-use QWS window. */
qwswl_window_t * qwswl_surface_to_win(struct wl_surface *surface);

qwswl_window_t * qwswl_find_window(qwswl_client_t *client, int32_t qws_id);

#ifdef __cplusplus
}
#endif

#endif /* WINDOW_H */
