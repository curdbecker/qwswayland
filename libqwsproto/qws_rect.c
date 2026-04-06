/*
 * qws_rect.c - Rectangle utilities for QWS rect arrays
 * SPDX-License-Identifier: MIT
 */

#include "qws_rect.h"

#include <pixman.h>
#include <stdlib.h>
#include <string.h>

void qws_rect_translate(qws_rect_t *rects, int32_t nrects, int32_t dx,
                        int32_t dy) {
    for (int32_t i = 0; i < nrects; i++) {
        rects[i].x1 += dx;
        rects[i].y1 += dy;
        rects[i].x2 += dx;
        rects[i].y2 += dy;
    }
}

void qws_rect_bounding_box(const qws_rect_t *rects, int32_t nrects,
                           int32_t *out_x1, int32_t *out_y1, int32_t *out_x2,
                           int32_t *out_y2) {
    *out_x1 = rects[0].x1;
    *out_y1 = rects[0].y1;
    *out_x2 = rects[0].x2;
    *out_y2 = rects[0].y2;
    for (int32_t i = 1; i < nrects; i++) {
        if (rects[i].x1 < *out_x1)
            *out_x1 = rects[i].x1;
        if (rects[i].y1 < *out_y1)
            *out_y1 = rects[i].y1;
        if (rects[i].x2 > *out_x2)
            *out_x2 = rects[i].x2;
        if (rects[i].y2 > *out_y2)
            *out_y2 = rects[i].y2;
    }
}

bool qws_rect_try_translate(qws_rect_t *rects, int32_t nrects, int32_t dx,
                            int32_t dy, int32_t max_x, int32_t max_y) {
    int32_t x1, y1, x2, y2;

    if (nrects <= 0)
        return false;

    qws_rect_bounding_box(rects, nrects, &x1, &y1, &x2, &y2);

    x1 += dx;
    x2 += dx;
    y1 += dy;
    y2 += dy;

    /* Do not translate a rect in a way that would violate
     * the global boundaries. */
    if (x1 < 0 || y1 < 0 || x1 > max_x || y1 > max_y || x2 < 0 || y2 < 0 ||
        x2 > max_x || y2 > max_y)
        return false;

    qws_rect_translate(rects, nrects, dx, dy);

    return true;
}

qws_rect_t *qws_rect_clone(const qws_rect_t *rects, int32_t nrects) {
    if (!rects || nrects <= 0)
        return NULL;

    qws_rect_t *copy = malloc((size_t)nrects * sizeof(qws_rect_t));
    if (!copy)
        return NULL;

    memcpy(copy, rects, (size_t)nrects * sizeof(qws_rect_t));
    return copy;
}

void qws_clip_rects(qws_rect_t *rects, int32_t nrects, int32_t max_x,
                    int32_t max_y) {
    for (int32_t i = 0; i < nrects; i++) {
        if (rects[i].x1 < 0)
            rects[i].x1 = 0;
        if (rects[i].y1 < 0)
            rects[i].y1 = 0;
        if (rects[i].x1 > max_x)
            rects[i].x1 = max_x;
        if (rects[i].y1 >= max_y)
            rects[i].y1 = max_y;

        if (rects[i].x2 < 0)
            rects[i].x2 = 0;
        if (rects[i].y2 < 0)
            rects[i].y2 = 0;
        if (rects[i].x2 >= max_x)
            rects[i].x2 = max_x;
        if (rects[i].y2 >= max_y)
            rects[i].y2 = max_y;
    }
}

qws_rect_t *qws_rect_subtract(const qws_rect_t *a, int32_t n_a,
                              const qws_rect_t *b, int32_t n_b,
                              int32_t *out_n) {
    *out_n = 0;

    if (!a || n_a <= 0)
        return NULL;

    if (!b || n_b <= 0) {
        *out_n = n_a;
        return qws_rect_clone(a, n_a);
    }

    /* pixman_box32_t is {int32_t x1,y1,x2,y2} — identical layout to qws_rect_t.
     * We can cast directly to avoid copying. */
    pixman_region32_t ra, rb, result;

    pixman_region32_init_rects(&ra, (const pixman_box32_t *)a, (int)n_a);
    pixman_region32_init_rects(&rb, (const pixman_box32_t *)b, (int)n_b);

    pixman_region32_init(&result);
    pixman_region32_subtract(&result, &ra, &rb);

    int n = 0;
    pixman_box32_t *boxes = pixman_region32_rectangles(&result, &n);

    qws_rect_t *out = NULL;
    if (n > 0) {
        out = malloc((size_t)n * sizeof(qws_rect_t));
        if (out) {
            memcpy(out, boxes, (size_t)n * sizeof(qws_rect_t));
            *out_n = (int32_t)n;
        }
    }

    pixman_region32_fini(&result);
    pixman_region32_fini(&rb);
    pixman_region32_fini(&ra);

    return out;
}