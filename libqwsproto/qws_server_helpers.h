/*
 * qws_server_helpers.h - Helper functions for building QWS server events
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_SERVER_HELPERS_H
#define QWS_SERVER_HELPERS_H

#include "qws_proto.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------
 * Construct common server→client event packets
 * All return a newly allocated packet (caller frees).
 * ----------------------------------------------------------- */

/* Build a Connected event to send to a newly connected client.
 * This is the very first thing the QWS server sends.
 * display_spec: e.g., ":0" (sent as rawData).
 * server_shm_id: SysV shm id for the shared display memory region.
 * client_id: unique id assigned to this client. */
qws_packet_t *qws_make_connected_event(int32_t client_id,
                                         int32_t server_shm_id,
                                         const char *display_spec);

/* Build a Creation event (ID range assignment).
 * object_id: first ID in the range.
 * count: how many consecutive IDs (object_id .. object_id+count-1). */
qws_packet_t *qws_make_creation_event(int32_t object_id, int32_t count);

/* Build a Region event: tell client about its allocated window region.
 * rects: array of qws_rect_t. nrects: count. */
qws_packet_t *qws_make_region_event(int32_t window, int32_t type,
                                      const qws_rect_t *rects, int32_t nrects);

/* Build a Mouse event. */
qws_packet_t *qws_make_mouse_event(int32_t window,
                                     int32_t x_root, int32_t y_root,
                                     int32_t state, int32_t delta, int32_t time_ms);

/* Build a Key event. */
qws_packet_t *qws_make_key_event(int32_t window,
                                   uint16_t unicode, uint32_t keycode,
                                   uint32_t modifiers,
                                   bool is_press, bool auto_repeat);

/* Build a Focus event. */
qws_packet_t *qws_make_focus_event(int32_t window, qws_focus_flag_t flag);

/* Build a MaxWindowRect event. */
qws_packet_t *qws_make_max_window_rect_event(int32_t window, int32_t x1, 
    int32_t y1, int32_t x2, int32_t y2);

/* Build a PropertyReply event. */
qws_packet_t *qws_make_property_reply(int32_t window, int32_t property,
                                        const void *data, int32_t len);

/* UTF-16 byte order for conversion functions */
typedef enum {
    QWS_UTF16_LE = 0,  /* little-endian, no BOM (default; matches QWS wire format on x86) */
    QWS_UTF16_BE = 1,  /* big-endian, no BOM (QDataStream default byte order) */
} qws_utf16_endian_t;

/* Convert UTF-16LE or UTF-16BE to UTF-8.
 * On success: *dst = malloc'd NUL-terminated string (caller frees), returns 0.
 * out_bytes may be NULL. On error: *dst = NULL, returns -1. */
int qws_convert_from_utf16(char **dst, const uint8_t *src, size_t srclen,
                                   qws_utf16_endian_t endian, size_t *out_bytes);

/* Convert UTF-8 to UTF-16LE or UTF-16BE.
 * On success: *dst = malloc'd buffer (caller frees), *out_bytes = byte count, returns 0.
 * out_bytes may be NULL. On error: *dst = NULL, returns -1. */
int qws_convert_to_utf16(uint8_t **dst, const char *src, size_t srclen,
                                 qws_utf16_endian_t endian, size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* QWS_SERVER_HELPERS_H */