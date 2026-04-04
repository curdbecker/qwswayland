/*
 * clipboard.c - Bridge between QWS property clipboard and wl_data_device.
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "clipboard.h"
#include "lifecycle.h"
#include "property_store.h"
#include "proxy.h"
#include "qws_event_factory.h"
#include "qws_proto.h"
#include "qws_unicode.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

/* ================================================================
 * wl_data_offer listener
 * ================================================================ */

static void offer_offer(void *data, struct wl_data_offer *offer,
                        const char *mime_type) {
    if (data)
        return; /* already have a preferred MIME type */
    if (strcmp(mime_type, "text/plain;charset=utf-8") != 0 &&
        strcmp(mime_type, "text/plain") != 0) {
        wl_data_offer_set_user_data(offer, NULL);
        return;
    }

    wl_data_offer_set_user_data(offer, strdup(mime_type));
}

static void offer_noop_source_actions(void *data, struct wl_data_offer *offer,
                                      uint32_t source_actions) {
    (void)data;
    (void)offer;
    (void)source_actions;
}

static void offer_noop_action(void *data, struct wl_data_offer *offer,
                              uint32_t dnd_action) {
    (void)data;
    (void)offer;
    (void)dnd_action;
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_noop_source_actions,
    .action = offer_noop_action,
};

/* ================================================================
 * wl_data_device listener
 * ================================================================ */

static void data_device_data_offer(void *data, struct wl_data_device *dev,
                                   struct wl_data_offer *offer) {
    (void)data;
    (void)dev;
    /* user_data starts NULL; offer_offer may update it to a strdup'd MIME. */
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
}

static void memfd_reset(qwswl_clipboard_t *cb) {
    if (cb->memfd_map) {
        munmap(cb->memfd_map, cb->memfd_size);
        cb->memfd_map = NULL;
        cb->memfd_size = 0;
    }
    if (cb->memfd >= 0) {
        assert(ftruncate(cb->memfd, 0) >= 0);
        assert(lseek(cb->memfd, 0, SEEK_SET) >= 0);
    }
}

static void release_offer(struct wl_data_offer *offer) {
    if (!offer)
        return;

    char *mime = wl_data_offer_get_user_data(offer);
    if (mime)
        free(mime);

    wl_data_offer_destroy(offer);
}

static void data_device_selection(void *data, struct wl_data_device *dev,
                                  struct wl_data_offer *offer) {
    (void)dev;
    qwswl_state_t *state = data;
    qwswl_clipboard_t *cb = &state->clipboard;

    /*
     * We are the data source right now, so it would be rather unwise for us to
     * trying to talk to ourselves in a blocking way, right?
     *
     * Who designed this interface? Seriously? This is just a collection of
     * different, subtle ways to shoot oneself in the foot over and over again.
     */
    if (cb->source)
        return;

    if (!offer)
        return;

    char *mime = wl_data_offer_get_user_data(offer);

    if (!mime) {
        release_offer(offer);
        return;
    }

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        fprintf(stderr, "[clipboard] pipe2 failed: %s\n", strerror(errno));
        release_offer(offer);
        return;
    }

    wl_data_offer_receive(offer, mime, pipefd[1]);

    /* Flush so the fd is delivered to the compositor via sendmsg before we
     * close our copy; the source can then start writing. */
    wl_display_flush(state->wl_display);
    close(pipefd[1]);

    /* Create or reuse the memfd that serves as our incoming clipboard buffer.
     */
    if (cb->memfd < 0) {
        cb->memfd = memfd_create("qwswl_clipboard", MFD_CLOEXEC);
        if (cb->memfd < 0) {
            fprintf(stderr, "[clipboard] memfd_create failed: %s\n",
                    strerror(errno));
            close(pipefd[0]);
            return;
        }
    }

    memfd_reset(cb);

    /* Splice the pipe into the memfd until EOF; accumulate byte count. */
    size_t total = 0;
    for (;;) {
        ssize_t n = splice(pipefd[0], NULL, cb->memfd, NULL, 65536,
                           SPLICE_F_MOVE | SPLICE_F_MORE);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(pipefd[0]);
    release_offer(offer);

    /* Remove the cached property if it exists */
    qwsprop_remove(&state->prop_store, 0, QWS_PROPERTY_TEXTCLIPBOARD);

    if (!total)
        return;

    /* Map the UTF-8 data. */
    void *map = mmap(NULL, total, PROT_READ, MAP_SHARED, cb->memfd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return;
    }
    cb->memfd_map = map;
    cb->memfd_size = total;

    /* Notify all connected QWS clients that the clipboard has new content. */
    qwswl_broadcast_property_notify(state, 0, QWS_PROPERTY_TEXTCLIPBOARD,
                                    QWS_PROP_NOTIFY_CHANGED);
}

static void data_device_noop_enter(void *data, struct wl_data_device *dev,
                                   uint32_t serial, struct wl_surface *surface,
                                   wl_fixed_t x, wl_fixed_t y,
                                   struct wl_data_offer *offer) {
    (void)data;
    (void)dev;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    (void)offer;
}

static void data_device_noop_leave(void *data, struct wl_data_device *dev) {
    (void)data;
    (void)dev;
}

static void data_device_noop_motion(void *data, struct wl_data_device *dev,
                                    uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data;
    (void)dev;
    (void)time;
    (void)x;
    (void)y;
}

static void data_device_noop_drop(void *data, struct wl_data_device *dev) {
    (void)data;
    (void)dev;
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_noop_enter,
    .leave = data_device_noop_leave,
    .motion = data_device_noop_motion,
    .drop = data_device_noop_drop,
    .selection = data_device_selection,
};

/* ================================================================
 * wl_data_source listener
 * ================================================================ */

static void source_send(void *data, struct wl_data_source *src,
                        const char *mime_type, int32_t fd) {
    (void)src;
    (void)mime_type;
    qwswl_state_t *state = data;
    qwswl_clipboard_t *cb = &state->clipboard;

    if (cb->utf8 && cb->utf8_len > 0) {
        /* Write in a loop; the fd is a pipe so write may be short. */
        const char *p = cb->utf8;
        size_t rem = cb->utf8_len;
        while (rem > 0) {
            ssize_t n = write(fd, p, rem);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            p += (size_t)n;
            rem -= (size_t)n;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *src) {
    qwswl_state_t *state = data;
    qwswl_clipboard_t *cb = &state->clipboard;
    wl_data_source_destroy(src);
    if (cb->source == src)
        cb->source = NULL;
}

static void source_noop_target(void *data, struct wl_data_source *src,
                               const char *mime_type) {
    (void)data;
    (void)src;
    (void)mime_type;
}

static void source_noop_dnd_drop_performed(void *data,
                                           struct wl_data_source *src) {
    (void)data;
    (void)src;
}

static void source_noop_dnd_finished(void *data, struct wl_data_source *src) {
    (void)data;
    (void)src;
}

static void source_noop_action(void *data, struct wl_data_source *src,
                               uint32_t dnd_action) {
    (void)data;
    (void)src;
    (void)dnd_action;
}

static const struct wl_data_source_listener source_listener = {
    .target = source_noop_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_noop_dnd_drop_performed,
    .dnd_finished = source_noop_dnd_finished,
    .action = source_noop_action,
};

/* ================================================================
 * Public API
 * ================================================================ */

void qwswl_clipboard_init(qwswl_state_t *state) {
    qwswl_clipboard_t *cb = &state->clipboard;

    if (cb->manager && state->wl_seat && !cb->device) {
        cb->device =
            wl_data_device_manager_get_data_device(cb->manager, state->wl_seat);
        wl_data_device_add_listener(cb->device, &data_device_listener, state);
    }
}

void qwswl_clipboard_set(qwswl_state_t *state, const void *utf16le,
                         int32_t len) {
    qwswl_clipboard_t *cb = &state->clipboard;

    qwsprop_set(&state->prop_store, 0, QWS_PROPERTY_TEXTCLIPBOARD,
                QWS_PROP_REPLACE, utf16le, len);

    /* Notify all connected QWS clients that the clipboard has new content.
     * Including the client that just decided to set the clipboard.
     *
     * Yes, this looks easy, but also wasteful and a source of potential
     * circular dependencies, but you know what if Wayland is allowed to be lazy
     * on the compositor side, so are we...*/
    qwswl_broadcast_property_notify(state, 0, QWS_PROPERTY_TEXTCLIPBOARD,
                                    QWS_PROP_NOTIFY_CHANGED);

    char *utf8 = NULL;
    size_t utf8_len = 0;
    /* Due to QWS being just messy and switching arbitrarily between byte count
     * and character count, we simply obtain the character count for UTF-16 by
     * dividing the byte count by 2 here. */
    if (qws_convert_from_utf16(&utf8, utf16le, (size_t)len / 2, QWS_UTF16_LE,
                               &utf8_len) != 0) {
        fprintf(stderr, "[clipboard] UTF-16→UTF-8 conversion failed\n");
        return;
    }

    if (cb->utf8)
        free(cb->utf8);
    cb->utf8 = utf8;
    cb->utf8_len = utf8_len;

    if (cb->source) {
        wl_data_source_destroy(cb->source);
        cb->source = NULL;
    }

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(cb->manager);
    wl_data_source_add_listener(src, &source_listener, state);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_device_set_selection(cb->device, src, state->last_input_serial);
    wl_display_flush(state->wl_display);
    cb->source = src;
}

void qwswl_clipboard_get(qwswl_state_t *state, const void **data_out,
                         int32_t *len_out) {
    qwswl_clipboard_t *cb = &state->clipboard;

    /* Try to retrieve cached result directly from the property store */
    bool cached = qwsprop_get(&state->prop_store, 0, QWS_PROPERTY_TEXTCLIPBOARD,
                              data_out, len_out);

    if (cached || (!cached && !cb->device))
        /* We either have already data, so there is nothing to do for us
         * here or the Wayland clipboard is not supported by the compositor, so
         * if there is nothing in the cache, there is nothing there at all */
        return;

    /*
     * It may seem a bit weird that we're not doing the UTF16 conversion right
     * when we receive the selection request, since we're also moving the
     * available data into a private cache. There are two reasons:
     *
     * - Wayland's clipboard:
     *
     * Like basically the rest of it, the clipboard is a nice idea, but a bit
     * too to much and too confusing (at least for my taste) and caching offers
     * and only retrieving them when the QWS client asks lead to weird effects.
     * There is also some uncertainty how long another Wayland client should
     * keep the data available, so splicing should offer an efficient way to get
     * the data into a private store (although not really a terribly
     * memory-efficient one).
     *
     * - "Do the least amount of work possible as long it is possible"
     *
     * The reason is that I am not that found of a lot of applications that
     * become very very laggy when dealing with large clipboard contents.
     * Therefore, we're trying to avoid all unnecessary operations as best as
     * possible until we are really required too. And I would have already
     * preferred to not having to deal with the clipboard contents at all until
     * a client is interested.
     */
    if (!cb->memfd_map)
        return;

    uint8_t *utf16 = NULL;
    size_t utf16_len = 0;
    if (qws_convert_to_utf16(&utf16, cb->memfd_map, cb->memfd_size,
                             QWS_UTF16_LE, &utf16_len) != 0) {
        fprintf(stderr, "[clipboard] UTF-8→UTF-16 conversion failed\n");
        return;
    }

    /* Raw UTF-8 source will be cached as UTF-16 in the property store, so no
     * need to keep the content of the memfd available and mapped around
     * anymore. */
    memfd_reset(cb);

    /* Replace it directly in the property store without going through another
     * unnecessary memcpy again. */
    qwsprop_add(&state->prop_store, 0, QWS_PROPERTY_TEXTCLIPBOARD);
    qwsprop_replace_internal(&state->prop_store, 0, QWS_PROPERTY_TEXTCLIPBOARD,
                             utf16, utf16_len);

    *data_out = utf16;
    *len_out = utf16_len;
}

void qwswl_clipboard_destroy(qwswl_state_t *state) {
    qwswl_clipboard_t *cb = &state->clipboard;

    if (cb->source) {
        wl_data_source_destroy(cb->source);
        cb->source = NULL;
    }
    if (cb->device) {
        wl_data_device_destroy(cb->device);
        cb->device = NULL;
    }
    free(cb->utf8);
    cb->utf8 = NULL;
    cb->utf8_len = 0;

    memfd_reset(&state->clipboard);
    if (cb->memfd >= 0) {
        close(cb->memfd);
        cb->memfd = -1;
    }

    if (cb->manager) {
        wl_data_device_manager_destroy(cb->manager);
        cb->manager = NULL;
    }
}
