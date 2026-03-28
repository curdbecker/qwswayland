
/*
 * window.c - QWSWayland proxy window/surface handling
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "window.h"
#include "lifecycle.h"
#include "client.h"
#include "qws_server_helpers.h"
#include "qws_trace.h"
#include "qws_rect.h"

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
}

static void xdg_toplevel_configure_bounds(void *data,
    struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)
{
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
    if (win->server_shm.buffer)
        wl_buffer_destroy(win->server_shm.buffer);

    if (win->server_shm.pixels && win->server_shm.size > 0)
        munmap(win->server_shm.pixels, win->server_shm.size);

    if (win->server_shm.fd >= 0)
        close(win->server_shm.fd);
    
    if (win->server_shm.pool)
        wl_shm_pool_destroy(win->server_shm.pool);
}

void qwswl_attach_client_shm(qwswl_window_t *win,
     int shm_id, int32_t width, int32_t height)
{
    if (win->client_shm.shm.shm_id == shm_id || shm_id == -1)
        return;

    qws_shm_detach(&win->client_shm.shm);
    assert(qws_shm_attach_sysv(&win->client_shm.shm, shm_id) == 0);
}

void qwswl_detach_client_shm(qwswl_window_t *win)
{
    assert(win->client_shm.shm.shm_id != -1);

    qws_shm_detach(&win->client_shm.shm);

    win->client_shm.format = -1;
}

static int qwswl_resize_buffer(qwswl_state_t *state, qwswl_window_t *win, 
    int32_t stride, size_t size, int32_t width, int32_t height)
{
    void *pixels = win->server_shm.pixels;
    assert(pixels != NULL);

    /* Update our information about the shared memory region. 
     * the region will likely never get smaller, since I am not sure 
     * SysV regions can actually be resized. In this case, the QWS client
     * will rather destroy and recreate it. We will be informed about
     * this explictely via region events then */
    if (qws_shm_update_sysv(&win->client_shm.shm) != 0) {
        return -1;
    }

    if (win->server_shm.size < size) {
        /* We need to resize the backing file, our memory mapping
         * and force Wayland to do a remap on its end. */

        if (ftruncate(win->server_shm.fd, (off_t)size) < 0)
            return -1;

        pixels = mremap(win->server_shm.pixels, 
            win->server_shm.size, size, MREMAP_MAYMOVE);
        if (pixels == MAP_FAILED)
            return -1;

        wl_shm_pool_resize(win->server_shm.pool, size);

        win->server_shm.size = size;
    }

    if (win->wl_surface) {
        /* we need to make sure that the compositor isn't currently
         * using the buffer, so we need commit and force it to process 
         * all outstanding events */
        wl_surface_commit(win->wl_surface);
        wl_display_roundtrip(state->wl_display);
    }

    /* We need to update the buffer in any case, since we need to associate 
     * the window dimensions with the buffer. Otherwise, there might be some
     * really strange display artifacts about to happen. */
    wl_buffer_destroy(win->server_shm.buffer);

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        win->server_shm.pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);

    win->server_shm.pixels = pixels;
    win->server_shm.buffer = buffer;

    if (!buffer) {
        return -1;
    }

    return 0;
}

static int qwswl_create_or_update_buffer(qwswl_state_t *state, qwswl_window_t *win,
                          int32_t width, int32_t height)
{
    int32_t stride = width * 4;  /* ARGB32 = 4 bytes/pixel */
    size_t size = (size_t)(stride * height);

    if (win->server_shm.pixels != NULL) {
        /* need to adjust the mapping of the existing buffer
         * in order to correctly represent the memory region to wayland */
        return qwswl_resize_buffer(state, win, stride, size, width, height);
    }
    
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
    if (!buffer) {
        munmap(pixels, size);
        close(fd);
        return -1;
    }

    win->server_shm.pool   = pool;
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
                                qws_rect_t *rects, int32_t nrects)
{
    if (!win || nrects <= 0 || !rects) return;

    /* Clip rects in place so we can echo them back to the client */
    qws_rect_constrain_to_screen(rects, nrects, state->screen_width, state->screen_height);

    /* Window geometry updates shall reset x and y position based on the
     * given geometry.
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
     * we will recalculate the x and y position when we receive a RegionMoveCommand
     * but then reset them here when we calculate the actual position.
     *
     * This hopefully does reflect how Qt would handle such cases internally.
     * The only uncertainty is whether a subsequent RegionCommand for an
     * already existing region will actually reset the offsets...
     */
    int32_t min_x1, min_y1, max_x2, max_y2;
    qws_rect_bounding_box(rects, nrects, &min_x1, &min_y1, &max_x2, &max_y2);
    win->geometry.x      = min_x1;
    win->geometry.y      = min_y1;
    win->geometry.width  = max_x2 - min_x1 + 1;
    win->geometry.height = max_y2 - min_y1 + 1;

    /* Store a copy of the (now clipped) rects for re-use in move acks */
    if (win->geometry.rects != rects) {
        win->geometry.rects = realloc(win->geometry.rects,
            (size_t)nrects * sizeof(qws_rect_t));
        assert(win->geometry.rects);
        memcpy(win->geometry.rects, rects, (size_t)nrects * sizeof(qws_rect_t));
        win->geometry.nrects = nrects;
    }

    if (win->parent)
        position_window_relative_to_parent(win);

    /* we may need to resize/create the buffer as well */
    assert(qwswl_create_or_update_buffer(state, win,
        win->geometry.width, win->geometry.height) == 0);
}

bool qwswl_move_window(qwswl_state_t *state, qwswl_window_t *win,
                       int32_t dx, int32_t dy)
{
    if (!win->geometry.rects)
        return false;   /* no valid region yet — ignore - see also above */

    /* Only toplevel windows can be moved; subsurface moves might need special
     * consideration regarding coordinate space and parent relationships
     * and does not intuitvely make that much sense. */
    assert(!win->parent);

    /* The offset is meant to be additive as the source code references
     * found by Claude and my intuition thought, so have to apply them
     * by re-translating the existing rects. */
    qws_rect_translate(win->geometry.rects, win->geometry.nrects, dx, dy);

    /* Re-calculate geometry after translating stored rects */
    qwswl_update_geometry(state, win, win->geometry.rects, 
        win->geometry.nrects);

    /* We need to ignore rectangles in repaint events for the first repaint
     * command after a move, since the referring to the previous window position
     * for some reason. See comment in qwswl_update_geometry below. */
    win->force_full_repaint = true;

    return true;
}

static void draw_debug_border(qwswl_window_t *win,
                              int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t color)
{
    static const int32_t RIM = 2; /* border thickness in pixels */

    if (w <= 0 || h <= 0) return;
    uint32_t *pixels = (uint32_t *)win->server_shm.pixels;
    int32_t  stride  = win->geometry.width;

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
    /* TODO: format conversion (e.g. RGB16 → ARGB32) when 
     * client_shm.format != ARGB32 */
    const size_t bytes_per_pixel = 4;

    if (!rects || nrects <= 0)
        return;

    const void *src = win->client_shm.shm.base;
    if (!src)
        return;

    /* should not try to update surface if the window does not exist */
    assert(win->wl_surface);

    /* assert that the QWS shm size and the WL buffer size can contain
     * the window surface */
    {
        size_t surface_bytes = 
            win->geometry.width * win->geometry.height * bytes_per_pixel;
        assert(win->client_shm.shm.size >= surface_bytes 
            && win->server_shm.size >= surface_bytes);
    }

    /* 
     * We are not going to entertain this nonsense anymore. 
     *
     * The QWS protocol seems to be filled with nasty habits that 
     * also show up with an unmodified QWS server after dumping
     * the conversation with our example client. This can cause
     * quite some severe confusion about the shared understanding
     * of the coordinate space.
     * 
     * This aspect is one of the very critical ones to understand
     * or it will cause confusion for days:
     * 
     * The RegionMove command physically moves the window on the screen
     * and thereby also translates the rects reported by the client
     * to the server as expected by the move, e.g. when a rect is moved
     * by (dx,dy), then all coordinates in each rect are also moved by
     * (dx,dy). And this is quite expected.
     * 
     * Unfortunately however, that's when it gets then really annoying,
     * since the first repaint event actually then still refers to the 
     * old position of the window. This might be a way for the client
     * to let the server know that after moving the window, the
     * old position needs to be repainted? Although this is quite
     * unnecessary, since the RegionMove command tells the server 
     * everything it needs to know.
     * 
     * For us this is even more inconvenient, since we then are getting
     * very confused when trying to map the global screen coordinates
     * given by the event again into Wayland surface local coordinates.
     * There is no surface anymore at the old coordinates, since we
     * already moved it, so we need to expect and deal with this.
     * 
     * Therefore, we're going to simply ignore the given rects on the
     * first RepaintRegion after a move (or whenever this might might be
     * required as well) and force a full repaint of the window given its
     * translated rects;
     */
    if (win->force_full_repaint) {
        rects = win->geometry.rects;
        nrects = win->geometry.nrects;
        win->force_full_repaint = false;
    }

    for (int i = 0; i < nrects; i++) {
        int32_t copy_x = rects[i].x1 - win->geometry.x;
        int32_t copy_y = rects[i].y1 - win->geometry.y;
        int32_t copy_w = rects[i].x2 - rects[i].x1 + 1;
        int32_t copy_h = rects[i].y2 - rects[i].y1 + 1;

    /* 
     * Some deeper debugging in order to quickly find out what the actual
     * issue would cause a buffer under- or overflow. 
     * 
     * We use this ugly-ish define here (thanks Claude btw), so that we can 
     * simply disable the assert during debugging nd still observe the buffer
     * issues while interacting with the windows.
     * 
     * Often this also leads to interesting clues regarding what window areas
     * are not drawn - or what rect areas are missing when rect drawing
     * debugging is enabled below.
     */
#define DBG_RECT_ASSERT(cond)                                                   \
    do                                                                          \
    {                                                                           \
        if (cond)                                                               \
        { \
            QWS_TRACE("update_surface skip rect[%d]: " #cond \
                " (copy_x=%d copy_y=%d copy_w=%d copy_h=%d win_w=%d win_h=%d)", \
                i, copy_x, copy_y, copy_w, copy_h, \
                win->geometry.width, win->geometry.height); \
            assert(!(cond)); \
        } } while(0)
        DBG_RECT_ASSERT(copy_x < 0);
        DBG_RECT_ASSERT(copy_y < 0);
        DBG_RECT_ASSERT(copy_w < 0);
        DBG_RECT_ASSERT(copy_h < 0);
        DBG_RECT_ASSERT(copy_x >= win->geometry.width);
        DBG_RECT_ASSERT(copy_y >= win->geometry.height);
        DBG_RECT_ASSERT(copy_w > win->geometry.width);
        DBG_RECT_ASSERT(copy_h > win->geometry.height);
        DBG_RECT_ASSERT((copy_x + copy_w) > win->geometry.width);
        DBG_RECT_ASSERT((copy_y + copy_h) > win->geometry.height);
#undef DBG_RECT_ASSERT

        uint32_t row_offset = copy_x * bytes_per_pixel;
        uint32_t row_bytes = win->geometry.width * bytes_per_pixel;

        for (int32_t j = 0; j < copy_h; j++) {
            uint32_t y = copy_y + j;
            if (y > win->geometry.height)
                break;
    
            uint32_t off = row_offset + (y * row_bytes);
            memcpy((uint8_t *) win->server_shm.pixels + off,
                   (const uint8_t *) src + off,
                   copy_w * bytes_per_pixel);
        }

        if (state->debug_draw_rects)
            draw_debug_border(win, copy_x, copy_y, copy_w, copy_h, 0xFFFF0000);

        wl_surface_damage(win->wl_surface, copy_x, copy_y, copy_w, copy_h);
    }

    if (state->debug_draw_rects)
        draw_debug_border(win, 0, 0,
                          win->geometry.width, win->geometry.height,
                          0xFFFFFF00); /* yellow — full window geometry */

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

    if (win->geometry.rects)
        free(win->geometry.rects);

    /* Release the permanently-attached client shm */
    qws_shm_detach(&win->client_shm.shm);

    /* mark the window as unused */
    qwswl_remove_window_from_client(win->client, win->qws_id);

    free(win);
}

void qwswl_hide_window(qwswl_state_t *state, qwswl_window_t *win)
{
    qwswl_detach_client_shm(win);

    win->geometry.x = 0;
    win->geometry.y = 0;
    win->geometry.height = -1;
    win->geometry.width = -1;
    if (win->geometry.rects)
        free(win->geometry.rects);
    win->geometry.rects  = NULL;
    win->geometry.nrects = 0;

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