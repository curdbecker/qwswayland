
/*
 * window.c - QWSWayland proxy window/surface handling
 * SPDX-License-Identifier: MIT
 */

#include "window.h"
#include "lifecycle.h"
#include "client.h"
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

#include "stc/common.h"
#include "stc/sys/utility.h"

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
    /* width/height of 0 means "compositor defers to the client" */
    assert (width == 0 && height == 0);
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    // qwswl_window_t *win = (qwswl_window_t *)data;
    // win->visible = false;
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
     int shm_id, int32_t width, int32_t height)
{
    if (win->client_shm.shm.shm_id != shm_id && shm_id != -1) {
        qws_shm_detach(&win->client_shm.shm);
        assert(qws_shm_attach_sysv(&win->client_shm.shm, shm_id) == 0);
    }
    win->client_shm.width  = width;
    win->client_shm.height = height;
}

void qwswl_detach_client_shm(qwswl_window_t *win)
{
    assert(win->client_shm.shm.shm_id != -1);

    qws_shm_detach(&win->client_shm.shm);

    win->client_shm.format = -1;
    win->client_shm.width  = -1;
    win->client_shm.height = -1;
}

static int qwswl_create_or_update_buffer(qwswl_state_t *state, qwswl_window_t *win,
                          int32_t width, int32_t height)
{
    int32_t stride = width * 4;  /* ARGB32 = 4 bytes/pixel */
    size_t size = (size_t)(stride * height);

    if (win->server_shm.pixels != NULL 
        && win->server_shm.size >= size)
        /* buffer exists and is not meant be changed - nothing to do here */
        return 0;
    
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

static void position_window_relative_to_parent(qwswl_window_t *win)
{
    qwswl_window_t *parent = win->parent;
    
    assert(parent && win->wl_subsurface);

    /* for lower-level windows the position of the subsurface is relative to
     * the parent, so we need to compensate the offset between the two windows */
    wl_subsurface_set_position(win->wl_subsurface, 
        win->geometry.x - parent->geometry.x, 
        win->geometry.y - parent->geometry.y);
    
    /* In order to avoid weird effects e.g. after the creation of an asynchronous
     * subsurface, we have to manually trigger an update on the parent, so
     * that our position relative to the parent surface is guranteed also
     * to be known in the parent surface. */
    wl_surface_commit(parent->wl_surface);
}

void qwswl_update_geometry(qwswl_state_t *state, qwswl_window_t *win,
                                const qws_rect_t *rects, int32_t nrects)
{
    if (!win || nrects <= 0 || !rects) return;

    /* Window geometry updates shall reset region movement offsets.
     *
     * The QWS implementation has the nasty habit of sometimes producing 
     * out of order commands that are apparently meant to be without 
     * any effect.
     * 
     * A RegionMoveCommand before a RegionCommand is expected to be
     * a no-operation that does should not affect coordinate translation.
     * 
     * Otherwise, this could then lead to incorrect pointer offsets that
     * inexplicably only affect certain windows. For instance, a
     * QMessageBox::about works without any issues and a QMessageBox::warning
     * suddenly does have a severe offset without any reasonable explanation.
     * 
     * In order to deal with this for now without any particular effort,
     * we will reset update the offsets when we receive a RegionMoveCommand
     * but then reset them here when we calculate calculate the actual position.
     * 
     * This hopefully does reflect how Qt would handle such cases internally.
     * The only uncertainty is whether a subsequent RegionCommand for an
     * already existing region will actually reset the offsets...
     */
    win->geometry.move_off_x = 0;
    win->geometry.move_off_y = 0;

    /* Compute bounding box of the region */
    int32_t min_x1 = rects[0].x1, min_y1 = rects[0].y1;
    int32_t max_x2 = rects[0].x2, max_y2 = rects[0].y2;

    for (int32_t i = 1; i < nrects; i++) {
        if (rects[i].x1 < min_x1) min_x1 = rects[i].x1;
        if (rects[i].y1 < min_y1) min_y1 = rects[i].y1;
        if (rects[i].x2 > max_x2) max_x2 = rects[i].x2;
        if (rects[i].y2 > max_y2) max_y2 = rects[i].y2;
    }

    int32_t width = max_x2 - min_x1 + 1;
    int32_t height = max_y2 - min_y1 + 1;

    if (min_x1 == win->geometry.x && min_y1 == win->geometry.y &&
            width == win->geometry.width && height == win->geometry.height)
        return;

    win->geometry.x = min_x1;
    win->geometry.y = min_y1;
    win->geometry.width = width;
    win->geometry.height = height;

    QWS_TRACE("Updating Window %d geometry", win->qws_id);

    if (win->parent)
        position_window_relative_to_parent(win);

    /* we need might to need to resize/create the buffer as well */
    qwswl_create_or_update_buffer(state, win, 
        win->geometry.width, win->geometry.height);
}

static void draw_debug_border(qwswl_window_t *win,
                              int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t color)
{
    static const int32_t RIM = 2; /* border thickness in pixels */

    if (w <= 0 || h <= 0) return;
    uint32_t *pixels = (uint32_t *)win->server_shm.pixels;
    int32_t  stride  = win->client_shm.width;

    int32_t rim = c_min(RIM, c_min(w / 2, h / 2)); /* clamp for tiny rects */

    for (int32_t t = 0; t < rim; t++) {
        /* top and bottom bands */
        for (int32_t px = x; px < x + w; px++) {
            pixels[(y + t) * stride + px]           = color;
            pixels[(y + h - 1 - t) * stride + px]   = color;
        }
        /* left and right bands */
        for (int32_t py = y + t + 1; py < y + h - 1 - t; py++) {
            pixels[py * stride + x + t]             = color;
            pixels[py * stride + x + w - 1 - t]     = color;
        }
    }
}

void qwswl_update_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects)
{
    if (!rects || nrects <= 0)
        return;

    const void *src = win->client_shm.shm.base;
    if (!src)
        return;

    /* should not try to update surface if the window does not exist */
    assert(win->wl_surface);

    /* buffer sizes must be always equal */
    assert(win->client_shm.shm.size == win->server_shm.size);

    for (int i = 0; i < nrects; i++) {
        int32_t copy_x = c_max(rects[i].x1- win->geometry.x + win->geometry.move_off_x, 0);
        int32_t copy_y = c_max(rects[i].y1 - win->geometry.y + win->geometry.move_off_y, 0);
        int32_t copy_w = c_min(rects[i].x2 - (rects[i].x1 + win->geometry.move_off_x) + 1, win->client_shm.width);
        int32_t copy_h = c_min(rects[i].y2 - (rects[i].y1 + win->geometry.move_off_y) + 1, win->client_shm.height);

        // int32_t copy_x = rects[i].x1 - win->geometry.x;
        // int32_t copy_y = rects[i].y1 - win->geometry.y;
        // int32_t copy_w = rects[i].x2 - rects[i].x1;
        // int32_t copy_h = rects[i].y2 - rects[i].y1;

        assert(copy_x >= 0 && copy_y >= 0);
        assert(copy_x <= win->client_shm.width && copy_y <= win->client_shm.height);
        assert(copy_w <= win->client_shm.width && copy_h <= win->client_shm.height);

        uint32_t row_offset = copy_x * 4;
        uint32_t row_bytes = win->client_shm.width * 4;

        /* TODO: format conversion (e.g. RGB16 → ARGB32) when client_shm.format != ARGB32 */
        for (int32_t j = 0; j < copy_h; j++) {
            uint32_t y = copy_y + j;
            if (y > win->client_shm.height)
                break;
    
            uint32_t off = row_offset + (y * row_bytes);
            memcpy((uint8_t *) win->server_shm.pixels + off,
                   (const uint8_t *) src + off,
                   copy_w * 4);
        }

        if (state->debug_draw_rects)
            draw_debug_border(win, copy_x, copy_y, copy_w, copy_h, 0xFFFF0000);

        wl_surface_damage(win->wl_surface, copy_x, copy_y, copy_w, copy_h);
    }

    if (state->debug_draw_rects)
        draw_debug_border(win, 0, 0,
                          win->geometry.width, win->geometry.height,
                          0xFFFFFF00); /* yellow — full window geometry */

    // wl_surface_damage(win->wl_surface, 0, 0, win->geometry.width, win->geometry.height);

    wl_surface_attach(win->wl_surface, win->server_shm.buffer, 0, 0);

    if (win->parent)
        position_window_relative_to_parent(win);

    wl_surface_commit(win->wl_surface);
    wl_display_flush(state->wl_display);
}

/* ================================================================
 * Window management
 * ================================================================ */

qwswl_window_t *qwswl_allocate_window(qwswl_client_t *client)
{
    int32_t qws_id = ++client->next_window_id;

    qwswl_window_t *win= calloc(1, sizeof(*win));
    assert(win);

    win->client = client;
    win->qws_id = qws_id;
    win->win_flags = -1;
    win->fixed = false;

    qwswl_add_window_to_client(client, qws_id, win);

    fprintf(stderr, "[qwswayland] Allocated window %d for client %d\n",
            win->qws_id, client->client_id);

    return win;
}

void qwswl_create_window(qwswl_state_t *state, qwswl_window_t *win, 
    qwswl_window_t *parent, qws_window_flags_t window_flags)
{
    assert(win && win->wl_surface == NULL);

    win->win_flags = window_flags;

    /* Create Wayland surface */
    win->wl_surface = wl_compositor_create_surface(state->wl_compositor);

    if (QWS_IS_TOPLEVEL_TYPE(window_flags)) {
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
        win->parent = parent;
        win->wl_subsurface = wl_subcompositor_get_subsurface(
            state->wl_subcompositor, win->wl_surface, win->parent->wl_surface);

        /* Avoids weird flickering or missing updates when subsurfaces 
         * are not updated together with the parent surface. */
        wl_subsurface_set_desync(win->wl_subsurface);

        /* Make sure that the position of the subsurface is already set 
         * as soon it might get displayed or we might see it jumping around. */
        position_window_relative_to_parent(win);
    }

    wl_surface_set_user_data(win->wl_surface, (void *) win);
    wl_surface_commit(win->wl_surface);

    /* window should have been fully created */
    wl_display_flush(state->wl_display);
    wl_display_roundtrip(state->wl_display);

    fprintf(stderr, "[qwswayland] Created window %d for client %d\n",
            win->qws_id, win->client->client_id);
}

void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win)
{
    assert(win);

    fprintf(stderr, "[qwswayland] Destroying window %d\n", win->qws_id);

    release_server_shm(win);

    if (win->xdg_toplevel)
        xdg_toplevel_destroy(win->xdg_toplevel);

    if (win->xdg_surface)
        xdg_surface_destroy(win->xdg_surface);

    if (win->wl_subsurface)
        wl_subsurface_destroy(win->wl_subsurface);

    if (win->wl_surface)
        wl_surface_destroy(win->wl_surface);
    
    /* make sure that the compositor processes all events
     * regarding this window, so that we won't receive any stray events anymore */
    wl_display_roundtrip(state->wl_display);

    if (win->name)
        free(win->name);

    if (win->caption)
        free(win->caption);

    /* Release the permanently-attached client shm */
    qws_shm_detach(&win->client_shm.shm);

    /* mark the window as unused */
    qwswl_remove_window_from_client(win->client, win->qws_id);

    free(win);
}

void qwswl_hide_window(qwswl_state_t *state, qwswl_window_t *win)
{
    qwswl_detach_client_shm(win);

    win->geometry.x = -1;
    win->geometry.y = -1;
    win->geometry.height = -1;
    win->geometry.width = -1;

    win->geometry.move_off_x = 0;
    win->geometry.move_off_y = 0;

    /* The client might decide to hide a window that we didn't even show yet,
     * so we might not even have a surface. There is nothing to do here then. */
    if (win->wl_surface) {
        wl_surface_attach(win->wl_surface, NULL, 0, 0);
        wl_surface_commit(win->wl_surface);
        wl_display_flush(state->wl_display);
    }
}

void qwswl_set_window_name(qwswl_window_t *win, char *name, char *caption)
{
    assert(win && name && caption);

    if (win->name)
        free(win->name);
    win->name = name;

    if (win->caption)
        free(win->caption);
    win->caption = caption;

    if (win->xdg_toplevel)
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