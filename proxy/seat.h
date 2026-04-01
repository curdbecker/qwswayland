/*
 * seat.h - QWSWayland proxy - seat event handling 
 * SPDX-License-Identifier: MIT
 */

#ifndef SEAT_H 
#define SEAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qwswl_window qwswl_window_t;

/* Per-pointer tracking. */
typedef struct {
    qwswl_window_t *win;
    int32_t         gx;
    int32_t         gy;
    int32_t         button_state;
    uint32_t        serial;
} qwswl_pointer_state_t;

 /* Per-keyboard state. */
typedef struct {
    struct xkb_keymap *xkb_keymap;
    struct xkb_state  *xkb_state;
    qwswl_window_t    *win;
} qwswl_keyboard_state_t;

#ifdef __cplusplus
}
#endif

#endif /* SEAT_H */
