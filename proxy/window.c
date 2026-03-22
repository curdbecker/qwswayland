
/*
 * window.c - QWSWayland proxy window/surface handling
 * SPDX-License-Identifier: MIT
 */

#include "window.h"
#include "lifecycle.h"
#include "connection.h"
#include "qws_server_helpers.h"
#include "qws_trace.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <assert.h>

#include "xdg-shell-client-protocol.h"


/* ================================================================
 * xdg-shell surface/toplevel listeners
 * ================================================================ */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                   uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height,
                                    struct wl_array *states)
{
    (void)toplevel; (void)states;
    qwswl_window_t *win = (qwswl_window_t *)data;
    /* width/height of 0 means "compositor defers to the client" */
    assert (width == 0 && height == 0);
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    qwswl_window_t *win = (qwswl_window_t *)data;
    win->visible = false;
}

static void xdg_toplevel_configure_bounds(void *data,
    struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)
{
    qwswl_window_t *win = (qwswl_window_t *)data;
    qwswl_client_t *cl = win->client;

    /* Send MaxWindowRect */
    qws_packet_t *mwr = qws_make_max_window_rect_event(
        win->qws_id, 0, 0, width, height);
    qws_trace_packet(cl->client_id, mwr, true);
    qws_write_packet(cl->fd, mwr);
}

static void xdg_wm_capabilities(void *data,
    struct xdg_toplevel *xdg_toplevel, struct wl_array *capabilities)
{
    (void)data; (void)xdg_toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure        = xdg_toplevel_configure,
    .close            = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities  = xdg_wm_capabilities,
};

/* ================================================================
 * Pixel buffer management
 * ================================================================ */

static void release_server_shm(qwswl_window_t *win)
{
    if (win->server_shm.buffer) {
        wl_buffer_destroy(win->server_shm.buffer);
        win->server_shm.buffer = NULL;
    }
    if (win->server_shm.pixels && win->server_shm.size > 0) {
        munmap(win->server_shm.pixels, win->server_shm.size);
        win->server_shm.pixels = NULL;
    }
    if (win->server_shm.fd >= 0) {
        close(win->server_shm.fd);
        win->server_shm.fd = -1;
    }
}

void qwswl_attach_client_shm(qwswl_window_t *win,
     int shm_id,
                              int32_t width, int32_t height)
{
    if (win->client_shm.shm.shm_id != shm_id && shm_id != -1) {
        qws_shm_detach(&win->client_shm.shm);
        assert(qws_shm_attach_sysv(&win->client_shm.shm, shm_id) == 0);
    }
    win->client_shm.width  = width;
    win->client_shm.height = height;
}

int qwswl_create_buffer(qwswl_state_t *state, qwswl_window_t *win,
                          int32_t width, int32_t height)
{
    int32_t stride = width * 4;  /* ARGB32 = 4 bytes/pixel */
    size_t size = (size_t)(stride * height);

    if (!state->wl_shm || !win->wl_surface)
        return -1;

    if (win->server_shm.pixels == NULL 
        && width == win->server_shm.size == size)
        /* buffer exists and is not meant be changed - nothing to here */
        return 0;

    /* Release any existing server-side buffer */
    release_server_shm(win);

    /* Create anonymous file for wl_shm */
    char template[] = "/tmp/qwswl-shm-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    unlink(template);

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }

    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return -1;
    }

    /* Create wl_shm_pool and wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, (int32_t)size);
    if (!pool) {
        munmap(pixels, size);
        close(fd);
        return -1;
    }

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    if (!buffer) {
        munmap(pixels, size);
        close(fd);
        return -1;
    }

    win->server_shm.buffer = buffer;
    win->server_shm.pixels = pixels;
    win->server_shm.fd     = fd;
    win->server_shm.size   = size;

    return 0;
}

void qwswl_commit_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects)
{
    const qws_rect_t default_rect =
        { 0, 0, win->geometry.width, win->geometry.height };

    if (!win || !win->wl_surface || !win->server_shm.buffer)
        return;

    const void *src = win->client_shm.shm.base;
    if (!src)
        return;

    if (!rects) {
        assert(nrects == 0);
        nrects = 1;
        rects = &default_rect;
    }

    wl_surface_attach(win->wl_surface, win->server_shm.buffer, 0, 0);

    for (int i = 0; i < nrects; i++) {
        uint32_t copy_x = rects[i].x1;
        uint32_t copy_y = rects[i].y1;
        uint32_t copy_w = rects[i].x2 - rects[i].x1;
        uint32_t copy_h = rects[i].y2 - rects[i].y1;

        // if (win->parent) {
        //     qwswl_geometry_t *p_geometry = &win->parent->geometry;
        //     copy_x -= p_geometry->x;
        //     copy_y -= p_geometry->y;
        // }

        uint32_t row_offset = copy_x * 4;
        uint32_t row_bytes = win->geometry.width * 4;

        /* TODO: format conversion (e.g. RGB16 → ARGB32) when client_shm.format != ARGB32 */
        for (uint32_t y = 0; y < copy_h; y++) {
            uint32_t off = row_offset + ((copy_y + y) * row_bytes);
            memcpy((uint8_t *) win->server_shm.pixels + off,
                   (const uint8_t *) src + off,
                   copy_w * 4);
        }

        wl_surface_damage(win->wl_surface, copy_x, copy_y, copy_w, copy_h);
    }

    wl_surface_commit(win->wl_surface);
    // if (win->parent)
        // wl_surface_commit(win->parent->wl_surface);

    wl_display_flush(state->wl_display);
}

/* ================================================================
 * Window management
 * ================================================================ */

qwswl_window_t *qwswl_create_window(qwswl_state_t *state, qwswl_window_t *win, 
    qwswl_client_t *client, bool is_first)
{
    int32_t qws_id = ++client->next_window_id;
    win = &client->windows[qws_id - 1];

    win->client = client;
    win->server_shm.fd = -1;
    win->client_shm.shm.shm_id = -1;
    win->client_shm.shm.fd = -1;
    win->opacity = 255;
    win->qws_id = qws_id;

    /* Create Wayland surface */
    win->wl_surface = wl_compositor_create_surface(state->wl_compositor);

    if (is_first) {
        /* Toplevel window: wrap in xdg_surface + xdg_toplevel.
         * Listeners must be added before the initial commit so we don't
         * miss the configure event that the compositor sends in response. */
        win->xdg_surface  = xdg_wm_base_get_xdg_surface(state->xdg_wm_base,
                                                          win->wl_surface);
        win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
        xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
        xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);
    } else {
        /* Child window: attach as a subsurface of the root toplevel.
         * No xdg-shell role or configure handshake needed. */
        win->parent = &client->windows[0];
        win->wl_subsurface = wl_subcompositor_get_subsurface(
            state->wl_subcompositor, win->wl_surface, win->parent->wl_surface);
        // wl_subsurface_set_sync(win->wl_subsurface);
    }

    wl_surface_set_user_data(win->wl_surface, (void *) win);
    wl_surface_commit(win->wl_surface);

    fprintf(stderr, "[qwswayland] Created window %d for client %d\n",
            win->qws_id, client->client_id);
    return win;
}

void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win)
{
    if (!win) return;

    fprintf(stderr, "[qwswayland] Destroying window %d\n", win->qws_id);

    release_server_shm(win);

    if (win->xdg_toplevel)
        xdg_toplevel_destroy(win->xdg_toplevel);

    if (win->xdg_surface)
        xdg_surface_destroy(win->xdg_surface);

    if (win->wl_surface)
        wl_surface_destroy(win->wl_surface);

    /* Release the permanently-attached client shm */
    qws_shm_detach(&win->client_shm.shm);

    memset(win, 0, sizeof(*win));
    win->server_shm.fd = -1;
    win->client_shm.shm.shm_id = -1;
    win->client_shm.shm.fd = -1;
}

void qwswl_update_window_region(qwswl_state_t *state, qwswl_window_t *win,
                                const qws_rect_t *rects, int32_t nrects,
                                int32_t width, int32_t height)
{
    if (!win || nrects <= 0 || !rects) return;

    /* Compute bounding box of the region */
    int32_t min_x1 = rects[0].x1, min_y1 = rects[0].y1;
    int32_t max_x2 = rects[0].x2, max_y2 = rects[0].y2;

    for (int32_t i = 1; i < nrects; i++) {
        if (rects[i].x1 < min_x1) min_x1 = rects[i].x1;
        if (rects[i].y1 < min_y1) min_y1 = rects[i].y1;
        if (rects[i].x2 > max_x2) max_x2 = rects[i].x2;
        if (rects[i].y2 > max_y2) max_y2 = rects[i].y2;
    }

    win->geometry.x = min_x1;
    win->geometry.y = min_y1;
    win->geometry.width = width;
    win->geometry.height = height;

    if (win->parent) {
        qwswl_window_t *parent = win->parent;
        assert(win->wl_subsurface);

        /* for lower-level windows the position of the subsurface is relative to
         * the parent, so we need to compensate the offset between the two windows */
        wl_subsurface_set_position(win->wl_subsurface, 
            win->geometry.x - parent->geometry.x, 
            win->geometry.y - parent->geometry.y);
    }

    /* (Re)create the Wayland buffer to match new size */
    qwswl_create_buffer(state, win, win->geometry.width, win->geometry.height);
}

void qwswl_set_window_name(qwswl_state_t *state, qwswl_window_t *win,
                             const char *name, const char *caption)
{
    (void)state; (void)caption;
    if (!name || !win || !win->xdg_toplevel) return;

    xdg_toplevel_set_title(win->xdg_toplevel, caption);
}

/* -----------------------------------------------------------
 * Lookup helpers
 * ----------------------------------------------------------- */

/* Resolve a Wayland surface to its in-use QWS window. */
inline qwswl_window_t *
qwswl_surface_to_win(struct wl_surface *surface)
{
    if (!surface)
        return NULL;
    qwswl_window_t *win = (qwswl_window_t *)wl_surface_get_user_data(surface);
    return win;
}

inline qwswl_window_t *
qwswl_find_window(qwswl_client_t *client, int32_t qws_id)
{
    assert(qws_id < QWSWL_MAX_WINDOWS);
    if (qws_id > client->next_window_id)
        return NULL;
    return &client->windows[qws_id - 1];
}