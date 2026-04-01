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
typedef struct qwswl_state qwswl_state_t;

/* Per-pointer tracking. */
typedef struct {
    qwswl_window_t *win;
    int32_t gx;
    int32_t gy;
    int32_t button_state;
    uint32_t serial;
} qwswl_pointer_state_t;

/* Per-keyboard state. */
typedef struct {
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    qwswl_window_t *win;

    /* Key-repeat state (driven by timerfd in the main event loop). */
    int repeat_timerfd;   /* timerfd fd, -1 until lifecycle sets it up */
    int32_t repeat_rate;  /* events/sec from compositor (0 = disabled) */
    int32_t repeat_delay; /* ms before first repeat */
    uint32_t
        repeat_key; /* evdev keycode of currently repeating key (0 = none) */
    int16_t repeat_unicode; /* saved unicode codepoint for the repeat tick */
    uint32_t repeat_qt_key; /* saved Qt::Key value for the repeat tick */
} qwswl_keyboard_state_t;

/* Called by the event loop when the key-repeat timerfd fires. */
void qwswl_keyboard_repeat_tick(qwswl_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* SEAT_H */
