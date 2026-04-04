/*
 * cursor.c - Wayland cursor dispatch for the QWSWayland proxy
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "cursor.h"
#include "client.h"
#include "lifecycle.h"
#include "qws_cursor.h"
#include "qws_trace.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define T qwswl_cursor_store_t, int32_t, qwswl_cursor_entry_t
#define i_declared
#include "stc/hashmap.h"

#include "cursor-shape-v1-client-protocol.h"
#include <wayland-client.h>

/* ================================================================
 * wp_cursor_shape mapping
 * ================================================================ */

/* Returns the wp_cursor_shape_device_v1_shape value for a QWS cursor, or
 * 0 when no named shape exists (use bitmap fallback). */
static uint32_t qws_shape_to_wp(qws_cursor_shape_t id) {
    switch (id) {
    case QWS_CURSOR_ARROW:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    case QWS_CURSOR_CROSS:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case QWS_CURSOR_WAIT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case QWS_CURSOR_IBEAM:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case QWS_CURSOR_SIZE_VER:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case QWS_CURSOR_SIZE_HOR:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case QWS_CURSOR_SIZE_BDIAG:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    case QWS_CURSOR_SIZE_FDIAG:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    case QWS_CURSOR_SIZE_ALL:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
    case QWS_CURSOR_SPLIT_V:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE;
    case QWS_CURSOR_SPLIT_H:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    case QWS_CURSOR_POINTING_HAND:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case QWS_CURSOR_FORBIDDEN:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case QWS_CURSOR_WHATS_THIS:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
    case QWS_CURSOR_BUSY:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
    case QWS_CURSOR_OPEN_HAND:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    case QWS_CURSOR_CLOSED_HAND:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    case QWS_CURSOR_DRAG_COPY:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY;
    case QWS_CURSOR_DRAG_MOVE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
    case QWS_CURSOR_DRAG_LINK:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS;
    /* QWS_CURSOR_UP_ARROW: no CSS equivalent → bitmap fallback */
    /* QWS_CURSOR_BLANK:    handled separately via wl_pointer_set_cursor(NULL)
     */
    default:
        return 0;
    }
}

/* ================================================================
 * Per-client cursor store
 * ================================================================ */

void qwswl_cursor_store_drop_all(qwswl_cursor_store_t *store) {
    for (c_each(it, qwswl_cursor_store_t, *store)) {
        free(it.ref->second.data);
        free(it.ref->second.mask);
    }
    qwswl_cursor_store_t_drop(store);
}

void qwswl_cursor_define(qwswl_client_t *cl, int32_t id, int32_t width,
                         int32_t height, int32_t hot_x, int32_t hot_y,
                         const void *raw_data, int32_t raw_len) {
    if (width <= 0 || height <= 0 || !raw_data)
        return;

    int32_t plane = height * ((width + 7) / 8);
    if (raw_len < plane * 2) {
        QWS_TRACE("cursor: DEFINE_CURSOR id=%d short payload (%d < %d)", id,
                  raw_len, plane * 2);
        return;
    }

    uint8_t *data = malloc(plane);
    uint8_t *mask = malloc(plane);
    if (!data || !mask) {
        free(data);
        free(mask);
        return;
    }
    memcpy(data, raw_data, plane);
    memcpy(mask, (const uint8_t *)raw_data + plane, plane);

    qwswl_cursor_entry_t entry = {data, mask, width, height, hot_x, hot_y};

    qwswl_cursor_store_t_iter it =
        qwswl_cursor_store_t_find(&cl->cursor_store, id);
    if (it.ref != qwswl_cursor_store_t_end(&cl->cursor_store).ref) {
        free(it.ref->second.data);
        free(it.ref->second.mask);
        it.ref->second = entry;
    } else {
        qwswl_cursor_store_t_insert(&cl->cursor_store, id, entry);
    }
    QWS_TRACE("cursor: DEFINE_CURSOR id=%d %dx%d hot=(%d,%d)", id, width,
              height, hot_x, hot_y);
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Pool size: large enough for a 32x32 ARGB8888 cursor (largest QWS cursor). */
#define CURSOR_POOL_SIZE (32 * 32 * 4)

bool qwswl_cursor_init(qwswl_state_t *state) {
    assert(state->wl_compositor);
    assert(state->wl_shm);

    state->cursor.shm_fd = -1;

    state->cursor.surface = wl_compositor_create_surface(state->wl_compositor);
    if (!state->cursor.surface)
        return -1;

    int fd = memfd_create("qwswl-cursor-pool", MFD_CLOEXEC);
    if (fd < 0)
        goto err_surface;

    if (ftruncate(fd, CURSOR_POOL_SIZE) < 0)
        goto err_fd;

    void *pixels =
        mmap(NULL, CURSOR_POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        goto err_fd;

    struct wl_shm_pool *pool =
        wl_shm_create_pool(state->wl_shm, fd, CURSOR_POOL_SIZE);
    if (!pool)
        goto err_mmap;

    state->cursor.shm_fd = fd;
    state->cursor.shm_pool = pool;
    state->cursor.pixels = pixels;
    state->cursor.shm_size = CURSOR_POOL_SIZE;
    state->cursor.shm_buffer = NULL;

    if (state->cursor.shape_manager && state->wl_pointer) {
        state->cursor.shape_device = wp_cursor_shape_manager_v1_get_pointer(
            state->cursor.shape_manager, state->wl_pointer);
        QWS_TRACE("cursor: cursor_shape_device created");
    }

    QWS_TRACE("cursor: initialized (shape_protocol=%s)",
              state->cursor.shape_device ? "yes" : "no");
    return true;

err_mmap:
    munmap(pixels, CURSOR_POOL_SIZE);
err_fd:
    close(fd);
err_surface:
    wl_surface_destroy(state->cursor.surface);
    state->cursor.surface = NULL;
    return false;
}

void qwswl_cursor_shutdown(qwswl_state_t *state) {
    if (state->cursor.shape_device) {
        wp_cursor_shape_device_v1_destroy(state->cursor.shape_device);
        state->cursor.shape_device = NULL;
    }
    if (state->cursor.shape_manager) {
        wp_cursor_shape_manager_v1_destroy(state->cursor.shape_manager);
        state->cursor.shape_manager = NULL;
    }
    if (state->cursor.shm_buffer) {
        wl_buffer_destroy(state->cursor.shm_buffer);
        state->cursor.shm_buffer = NULL;
    }
    if (state->cursor.shm_pool) {
        wl_shm_pool_destroy(state->cursor.shm_pool);
        state->cursor.shm_pool = NULL;
    }
    if (state->cursor.pixels && state->cursor.shm_size > 0) {
        munmap(state->cursor.pixels, state->cursor.shm_size);
        state->cursor.pixels = NULL;
    }
    if (state->cursor.shm_fd >= 0) {
        close(state->cursor.shm_fd);
        state->cursor.shm_fd = -1;
    }
    if (state->cursor.surface) {
        wl_surface_destroy(state->cursor.surface);
        state->cursor.surface = NULL;
    }
}

/* ================================================================
 * Cursor selection
 * ================================================================ */

void qwswl_cursor_select(qwswl_state_t *state, qwswl_client_t *cl,
                         qws_cursor_shape_t cursor_id) {
    if (!state->wl_pointer)
        return;

    uint32_t enter_serial = state->pointer_state.enter_serial;

    /* Case 1: BlankCursor — pass NULL surface to hide the cursor. */
    if (cursor_id == QWS_CURSOR_BLANK) {
        wl_pointer_set_cursor(state->wl_pointer, enter_serial, NULL, 0, 0);
        QWS_TRACE("cursor: set blank");
        return;
    }

    /* Case 2: Named shape via wp_cursor_shape_v1 (preferred). */
    if (state->cursor.shape_device) {
        uint32_t shape = qws_shape_to_wp(cursor_id);
        if (shape != 0) {
            wp_cursor_shape_device_v1_set_shape(state->cursor.shape_device,
                                                enter_serial, shape);
            QWS_TRACE("cursor: set shape id=%d wp_shape=%u", (int)cursor_id,
                      shape);
            return;
        }
    }

    /* Case 3: Bitmap fallback (UpArrowCursor, or no shape protocol). */
    qws_cursor_bitmap_t bm;
    if (!qws_cursor_bitmap(cursor_id, &bm)) {
        /* Try client-defined cursor from DEFINE_CURSOR. */
        qwswl_cursor_store_t_iter it =
            qwswl_cursor_store_t_find(&cl->cursor_store, (int32_t)cursor_id);
        if (it.ref == qwswl_cursor_store_t_end(&cl->cursor_store).ref) {
            QWS_TRACE("cursor: no bitmap for id=%d, ignoring", (int)cursor_id);
            return;
        }
        qwswl_cursor_entry_t *e = &it.ref->second;
        bm.data = e->data;
        bm.mask = e->mask;
        bm.width = e->width;
        bm.height = e->height;
        bm.hot_x = e->hot_x;
        bm.hot_y = e->hot_y;
    }

    size_t needed = (size_t)(bm.width * bm.height * 4);
    if (needed > state->cursor.shm_size) {
        QWS_TRACE("cursor: bitmap too large (%zu > %zu) for id=%d", needed,
                  state->cursor.shm_size, (int)cursor_id);
        return;
    }

    if (!qws_cursor_to_argb(&bm, (uint32_t *)state->cursor.pixels)) {
        QWS_TRACE("cursor: argb conversion failed for id=%d", (int)cursor_id);
        return;
    }

    /* Destroy the previous wl_buffer before creating a new one.
     * The pool is fixed-size and stays alive. */
    if (state->cursor.shm_buffer) {
        wl_buffer_destroy(state->cursor.shm_buffer);
        state->cursor.shm_buffer = NULL;
    }

    state->cursor.shm_buffer = wl_shm_pool_create_buffer(
        state->cursor.shm_pool, 0, bm.width, bm.height, bm.width * 4,
        WL_SHM_FORMAT_ARGB8888);

    if (!state->cursor.shm_buffer) {
        QWS_TRACE("cursor: failed to create wl_buffer for id=%d",
                  (int)cursor_id);
        return;
    }

    wl_surface_attach(state->cursor.surface, state->cursor.shm_buffer, 0, 0);
    wl_surface_damage(state->cursor.surface, 0, 0, bm.width, bm.height);
    wl_surface_commit(state->cursor.surface);

    wl_pointer_set_cursor(state->wl_pointer, enter_serial,
                          state->cursor.surface, bm.hot_x, bm.hot_y);

    QWS_TRACE("cursor: set bitmap id=%d (%dx%d hot=%d,%d)", (int)cursor_id,
              bm.width, bm.height, bm.hot_x, bm.hot_y);
}
