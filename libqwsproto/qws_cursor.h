/*
 * qws_cursor.h - QWS system cursor bitmap descriptors and ARGB conversion
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_CURSOR_H
#define QWS_CURSOR_H

#include "qws_proto.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One 1bpp monochrome cursor (LSB-first packed bits, same layout as Qt's
 * static uchar arrays in qwscursor_qws.cpp). Stride = (width+7)/8. */
typedef struct {
    const uint8_t *data; /* shape plane: 1 = cursor pixel, 0 = background */
    const uint8_t *mask; /* mask plane:  1 = opaque,       0 = transparent */
    int width;
    int height;
    int hot_x;
    int hot_y;
} qws_cursor_bitmap_t;

/* Fill *out for system cursors that have bitmap data (ids 0-9, 11-18).
 * Returns false for QWS_CURSOR_BLANK (10), DnD ids 19-21, and custom ids. */
bool qws_cursor_bitmap(qws_cursor_shape_t id, qws_cursor_bitmap_t *out);

/* Convert a 1bpp monochrome cursor to ARGB8888.
 * argb_out must be caller-allocated: bm->width * bm->height * 4 bytes.
 * Pixel encoding:
 *   data=1 & mask=1 → 0xFF000000 (opaque black)
 *   data=0 & mask=1 → 0xFFFFFFFF (opaque white)
 *   mask=0          → 0x00000000 (transparent) */
bool qws_cursor_to_argb(const qws_cursor_bitmap_t *bm, uint32_t *argb_out);

#ifdef __cplusplus
}
#endif

#endif /* QWS_CURSOR_H */
