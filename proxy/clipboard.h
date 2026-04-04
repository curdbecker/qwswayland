/*
 * clipboard.h - Bridge between QWS property clipboard and wl_data_device.
 * SPDX-License-Identifier: MIT
 */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "qws_proto.h"

#include <stddef.h>
#include <stdint.h>

#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qwswl_state qwswl_state_t;

typedef struct {
    struct wl_data_device_manager *manager;
    struct wl_data_device *device;

    /* Outgoing: cached UTF-8 text served via wl_data_source.send */
    struct wl_data_source *source;
    char *utf8;
    size_t utf8_len;

    /* Reusable memfd backing the incoming clipboard buffer*/
    int memfd;         /* -1 until first use */
    size_t memfd_size; /* bytes written by last receive */
    void *memfd_map;   /* active mmap of memfd, NULL when not mapped */
} qwswl_clipboard_t;

/* Set up wl_data_device if Wayland clipboard is supported.
 * Can be called multiple times until manager + seat are both present. */
void qwswl_clipboard_init(qwswl_state_t *state);

/* Free all clipboard resources at proxy shutdown. */
void qwswl_clipboard_destroy(qwswl_state_t *state);

void qwswl_clipboard_set(qwswl_state_t *state, const void *utf16le,
                         int32_t len);

void qwswl_clipboard_get(qwswl_state_t *state, const void **data_out,
                         int32_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* CLIPBOARD_H */
