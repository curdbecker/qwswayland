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

/* Try to translate all rects in the array by (dx, dy), 
 * but only if the bounding box of all rects would not violate
 * the constraints given by the rect {(0,0), (max_x, max_y)}.
 * 
 * Returns true if the rects were translated in-place.
 * Returns false if a safe translation was not possible 
 * without any changes to the given rects. */
bool qws_rect_try_translate(qws_rect_t *rects, int32_t nrects,
                            int32_t dx, int32_t dy,
                            int32_t max_x, int32_t max_y);

/* Compute the bounding box of an array of rects.
 * Writes min(x1), min(y1), max(x2), max(y2) across all entries. */
void qws_rect_bounding_box(const qws_rect_t *rects, int32_t nrects,
                            int32_t *out_x1, int32_t *out_y1,
                            int32_t *out_x2, int32_t *out_y2);

/* Clip rects in place based on the bonding box rect
 * {(0,0), (max_x, max_y)}. */
void qws_clip_rects(qws_rect_t *rects, int32_t nrects,
    int32_t max_x, int32_t max_y);

/* Return a heap-allocated copy of the given rect array.
 * Caller is responsible for freeing the returned pointer.
 * Returns NULL if nrects <= 0 or rects is NULL. */
qws_rect_t *qws_rect_clone(const qws_rect_t *rects, int32_t nrects);

/* Compute A minus B: return a heap-allocated, YX-sorted array of rects
 * representing the area covered by A but not by B.
 * Caller is responsible for freeing the returned pointer.
 * Returns NULL and sets *out_n = 0 if the result is empty. */
qws_rect_t *qws_rect_subtract(const qws_rect_t *a, int32_t n_a,
                               const qws_rect_t *b, int32_t n_b,
                               int32_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* QWS_RECT_H */
