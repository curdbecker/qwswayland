/*
 * qws_event_factory.c - Server-side event construction helpers
 * SPDX-License-Identifier: MIT
 */

#include "qws_event_factory.h"
#include <stdlib.h>
#include <string.h>

#include <assert.h>

qws_packet_t *qws_make_connected_event(int32_t client_id, int32_t server_shm_id,
                                       const char *display_spec) {
    if (!display_spec)
        return NULL;

    size_t spec_len = strlen(display_spec) + 1; /* include null terminator */

    qws_packet_t *pkt = qws_packet_alloc(QWS_EVT_CONNECTED,
                                         sizeof(qws_evt_connected_t), spec_len);
    if (!pkt)
        return NULL;

    qws_evt_connected_t *d = (qws_evt_connected_t *)pkt->simple_data;
    d->window = 0;
    d->len = (int32_t)spec_len;
    d->client_id = client_id;
    d->server_shm_id = server_shm_id;

    memcpy(pkt->raw_data, display_spec, spec_len);

    return pkt;
}

qws_packet_t *qws_make_creation_event(int32_t object_id, int32_t count) {
    qws_packet_t *pkt =
        qws_packet_alloc(QWS_EVT_CREATION, sizeof(qws_evt_creation_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_creation_t *d = (qws_evt_creation_t *)pkt->simple_data;
    d->object_id = object_id;
    d->count = count;

    return pkt;
}

qws_packet_t *qws_make_region_event(int32_t window, int32_t type,
                                    const qws_rect_t *rects, int32_t nrects) {
    size_t raw_len = (size_t)nrects * sizeof(qws_rect_t);
    qws_packet_t *pkt =
        qws_packet_alloc(QWS_EVT_REGION, sizeof(qws_evt_region_t), raw_len);
    if (!pkt)
        return NULL;

    qws_evt_region_t *d = (qws_evt_region_t *)pkt->simple_data;
    d->window = window;
    d->nrectangles = nrects;
    d->type = type;

    if (nrects > 0 && rects) {
        memcpy(pkt->raw_data, rects, raw_len);
    }

    return pkt;
}

qws_packet_t *qws_make_mouse_event(int32_t window, int32_t x_root,
                                   int32_t y_root, int32_t state, int32_t delta,
                                   int32_t time_ms) {
    qws_packet_t *pkt =
        qws_packet_alloc(QWS_EVT_MOUSE, sizeof(qws_evt_mouse_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_mouse_t *d = (qws_evt_mouse_t *)pkt->simple_data;
    d->window = window;
    d->x_root = x_root;
    d->y_root = y_root;
    d->state = state;
    d->delta = delta;
    d->time = time_ms;

    return pkt;
}

qws_packet_t *qws_make_key_event(int32_t window, uint16_t unicode,
                                 uint32_t keycode, uint32_t modifiers,
                                 bool is_press, bool auto_repeat) {
    qws_packet_t *pkt = qws_packet_alloc(QWS_EVT_KEY, sizeof(qws_evt_key_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_key_t *d = (qws_evt_key_t *)pkt->simple_data;
    d->window = window;
    d->keycode = keycode;
    d->modifiers = modifiers;
    d->unicode = unicode;
    d->flags = (is_press ? QWS_KEY_FLAG_PRESS : 0u) |
               (auto_repeat ? QWS_KEY_FLAG_AUTO_REPEAT : 0u);

    return pkt;
}

qws_packet_t *qws_make_focus_event(int32_t window, qws_focus_flag_t flag) {
    qws_packet_t *pkt =
        qws_packet_alloc(QWS_EVT_FOCUS, sizeof(qws_evt_focus_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_focus_t *d = (qws_evt_focus_t *)pkt->simple_data;
    d->window = window;
    d->get_focus = flag;

    return pkt;
}

qws_packet_t *qws_make_max_window_rect_event(int32_t window, int32_t x1,
                                             int32_t y1, int32_t x2,
                                             int32_t y2) {
    qws_packet_t *pkt = qws_packet_alloc(QWS_EVT_MAX_WINDOW_RECT,
                                         sizeof(qws_evt_max_window_rect_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_max_window_rect_t *d =
        (qws_evt_max_window_rect_t *)pkt->simple_data;
    d->window = window;
    d->rect.x1 = x1;
    d->rect.y1 = y1;
    d->rect.x2 = x2;
    d->rect.y2 = y2;

    return pkt;
}

qws_packet_t *qws_make_property_notify(int32_t window, int32_t property,
                                       qws_prop_notify_state_t state) {
    qws_packet_t *pkt = qws_packet_alloc(QWS_EVT_PROPERTY_NOTIFY,
                                         sizeof(qws_evt_property_notify_t), 0);
    if (!pkt)
        return NULL;

    qws_evt_property_notify_t *d =
        (qws_evt_property_notify_t *)pkt->simple_data;
    d->window = window;
    d->property = property;
    d->state = state;

    return pkt;
}

qws_packet_t *qws_make_property_reply(int32_t window, int32_t property,
                                      const void *data, int32_t len) {
    qws_packet_t *pkt = qws_packet_alloc(QWS_EVT_PROPERTY_REPLY,
                                         sizeof(qws_evt_property_reply_t),
                                         (size_t)(len > 0 ? len : 0));
    if (!pkt)
        return NULL;

    qws_evt_property_reply_t *d = (qws_evt_property_reply_t *)pkt->simple_data;
    d->window = window;
    d->property = property;
    d->len = len;

    if (len > 0 && data) {
        memcpy(pkt->raw_data, data, (size_t)len);
    }

    return pkt;
}
