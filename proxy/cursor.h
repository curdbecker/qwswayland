/*
 * cursor.h - Wayland cursor dispatch for the QWSWayland proxy
 * SPDX-License-Identifier: MIT
 */

#ifndef CURSOR_H
#define CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>

#include "qws_proto.h"
#include "stc/types.h"

#include "cursor-shape-v1-client-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to avoid circular includes. */
typedef struct qwswl_state qwswl_state_t;
typedef struct qwswl_client qwswl_client_t;

/* All Wayland cursor state, embedded as qwswl_state_t::cursor. */
typedef struct {
    struct wp_cursor_shape_manager_v1 *shape_manager;
    struct wp_cursor_shape_device_v1 *shape_device;
    struct wl_surface *surface;
    struct wl_shm_pool *shm_pool;
    struct wl_buffer *shm_buffer;
    void *pixels; /* mmap base */
    int shm_fd;   /* memfd, -1 until cursor_init */
    size_t shm_size;
} qwswl_cursor_t;

/* Per-client custom cursor entry (from DEFINE_CURSOR). */
typedef struct {
    uint8_t *data; /* heap copy of shape plane (1bpp LSB-first) */
    uint8_t *mask; /* heap copy of mask plane  (1bpp LSB-first) */
    int32_t width, height, hot_x, hot_y;
} qwswl_cursor_entry_t;

declare_hashmap(qwswl_cursor_store_t, int32_t, qwswl_cursor_entry_t);

/* Allocate the reusable cursor surface and shm pool; create
 * cursor_shape_device if the protocol is available.
 * Call once after wl_compositor and wl_shm are bound. */
bool qwswl_cursor_init(qwswl_state_t *state);

/* Release all cursor resources. */
void qwswl_cursor_shutdown(qwswl_state_t *state);

/* Free all entries in a per-client cursor store and drop it. */
void qwswl_cursor_store_drop_all(qwswl_cursor_store_t *store);

/* Store a custom cursor from a DEFINE_CURSOR command.
 * raw_data layout: [shape_plane | mask_plane], each plane = height*(width+7)/8
 * bytes. */
void qwswl_cursor_define(qwswl_client_t *cl, int32_t id, int32_t width,
                         int32_t height, int32_t hot_x, int32_t hot_y,
                         const void *raw_data, int32_t raw_len);

/* Apply a SELECT_CURSOR command: set the compositor cursor to cursor_id.
 * Uses wp_cursor_shape_v1 when available, falls back to bitmap upload,
 * or hides the cursor for QWS_CURSOR_BLANK. */
void qwswl_cursor_select(qwswl_state_t *state, qwswl_client_t *cl,
                         qws_cursor_shape_t cursor_id);

#ifdef __cplusplus
}
#endif

#endif /* CURSOR_H */
