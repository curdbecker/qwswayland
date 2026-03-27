/*
 * proxy.h - QWSWayland proxy daemon command dispatch
 * SPDX-License-Identifier: MIT
 */

#ifndef SEATH_H 
#define SEATH_H

#include "window.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-pointer tracking: allocated on enter, freed on leave */
typedef struct {
    qwswl_window_t *win;
    int32_t         gx;
    int32_t         gy;
    int32_t         button_state;
} qwswl_pointer_data_t;


 /* Per-keyboard state: allocated on keymap event */
typedef struct {
    struct xkb_keymap *xkb_keymap;
    struct xkb_state  *xkb_state;
    qwswl_window_t    *win;
} qwswl_keyboard_data_t;

#ifdef __cplusplus
}
#endif

#endif /* SEATH_H */
