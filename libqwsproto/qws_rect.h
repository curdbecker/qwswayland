/*
 * qws_rect.h - Rectangle utilities for QWS rect arrays
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_RECT_H
#define QWS_RECT_H

#include "qws_proto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Translate all rects in the array by (dx, dy). */
void qws_rect_translate(qws_rect_t *rects, int32_t nrects,
                         int32_t dx, int32_t dy);

/* Compute the bounding box of an array of rects.
 * Writes min(x1), min(y1), max(x2), max(y2) across all entries. */
void qws_rect_bounding_box(const qws_rect_t *rects, int32_t nrects,
                            int32_t *out_x1, int32_t *out_y1,
                            int32_t *out_x2, int32_t *out_y2);

/* Clip rects in place: x1/y1 x2/y2 clamped to 0 and screen edge. */
void qws_clip_rects(qws_rect_t *rects, int32_t nrects, int32_t sw, int32_t sh);

/* Constrain rects to screen: first translates the group so that any off-screen
 * overhang is pushed back onto the screen (preserving relative positions), then
 * clips to screen dimensions to handle windows larger than the screen. */
void qws_rect_constrain_to_screen(qws_rect_t *rects, int32_t nrects,
                                   int32_t sw, int32_t sh);

#ifdef __cplusplus
}
#endif

#endif /* QWS_RECT_H */
