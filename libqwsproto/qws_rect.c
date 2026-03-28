/*
 * qws_rect.c - Rectangle utilities for QWS rect arrays
 * SPDX-License-Identifier: MIT
 */

#include "qws_rect.h"

void qws_rect_translate(qws_rect_t *rects, int32_t nrects,
                         int32_t dx, int32_t dy)
{
    for (int32_t i = 0; i < nrects; i++) {
        rects[i].x1 += dx; rects[i].y1 += dy;
        rects[i].x2 += dx; rects[i].y2 += dy;
    }
}

void qws_rect_bounding_box(const qws_rect_t *rects, int32_t nrects,
                            int32_t *out_x1, int32_t *out_y1,
                            int32_t *out_x2, int32_t *out_y2)
{
    *out_x1 = rects[0].x1; *out_y1 = rects[0].y1;
    *out_x2 = rects[0].x2; *out_y2 = rects[0].y2;
    for (int32_t i = 1; i < nrects; i++) {
        if (rects[i].x1 < *out_x1) *out_x1 = rects[i].x1;
        if (rects[i].y1 < *out_y1) *out_y1 = rects[i].y1;
        if (rects[i].x2 > *out_x2) *out_x2 = rects[i].x2;
        if (rects[i].y2 > *out_y2) *out_y2 = rects[i].y2;
    }
}

void qws_rect_constrain_to_screen(qws_rect_t *rects, int32_t nrects,
                                   int32_t sw, int32_t sh)
{
    if (nrects <= 0) return;

    int32_t x1, y1, x2, y2;
    qws_rect_bounding_box(rects, nrects, &x1, &y1, &x2, &y2);

    int32_t dx = 0, dy = 0;
    if      (x1 < 0)   dx = -x1;
    else if (x2 >= sw) dx = (sw - 1) - x2;
    if      (y1 < 0)   dy = -y1;
    else if (y2 >= sh) dy = (sh - 1) - y2;

    if (dx || dy)
        qws_rect_translate(rects, nrects, dx, dy);

    qws_clip_rects(rects, nrects, sw, sh);
}

/* Clip rects in place: x1/y1 x2/y2 clamped to 0 and screen edge. */
void qws_clip_rects(qws_rect_t *rects, int32_t nrects,
                       int32_t sw, int32_t sh)
{
    for (int32_t i = 0; i < nrects; i++) {
        if (rects[i].x1 < 0)      rects[i].x1 = 0;
        if (rects[i].y1 < 0)      rects[i].y1 = 0;
        if (rects[i].x1 >= sw)    rects[i].x1 = sw - 1;
        if (rects[i].y1 >= sh)    rects[i].y1 = sh - 1;

        if (rects[i].x2 < 0)      rects[i].x2 = 0;
        if (rects[i].y2 < 0)      rects[i].y2 = 0;
        if (rects[i].x2 >= sw)    rects[i].x2 = sw - 1;
        if (rects[i].y2 >= sh)    rects[i].y2 = sh - 1;
    }
}