/*
 * seat.c - QWSWayland proxy - seat event handling 
 * SPDX-License-Identifier: MIT
 */

#include "seat.h"
#include "window.h"
#include "lifecycle.h"
#include "client.h"
#include "qws_event_factory.h"
#include "qws_trace.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unicode/ustring.h>

#include <time.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>

#include <assert.h>
#include "debug.h"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "stc/common.h"
#include "stc/sys/utility.h"

/* ================================================================
 * Wayland pointer listener → QWS mouse events
 * ================================================================ */

static void send_pointer_update_event(qwswl_pointer_state_t *pstate,
    int32_t scroll_delta)
{
    qwswl_window_t *win = pstate->win;
    qwswl_client_t *cl = win->client;

    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    int32_t time = (int32_t)(_ts.tv_sec * 1000 + _ts.tv_nsec / 1000000);

    qws_packet_t *evt = qws_make_mouse_event(
        win->qws_id, pstate->gx, pstate->gy,
        pstate->button_state, scroll_delta, time);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void update_pointer_position(qwswl_state_t *state,
                                    wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    qwswl_window_t *win = pstate->win;
    int32_t gx, gy;

    /* Translate surface-local coords to QWS global coords */
    gx = win->geometry.x + wl_fixed_to_int(sx);
    gy = win->geometry.y + wl_fixed_to_int(sy);

#ifdef QWSWL_DEBUG_POINTER_MOVEMENT
    QWS_TRACE("qws_win=%d, surface_local=(%d,%d), global=(%d,%d)",
              win->qws_id, 
              wl_fixed_to_int(sx), wl_fixed_to_int(sy),
              gx, gy);
#endif

    /* Do not the same event multiple times - Qt does not do 
     * that as well. */
    if (gx == pstate->gx && gy == pstate->gy)
        return;

    pstate->gx = gx;
    pstate->gy = gy;

    /* This seems to be a bit redundant? Yes, I know, but
     * if we don't keep both positions updated, then we are
     * going to unleash a bunch of very very weird edge cases. */
    *state->qt_last_x = gx;
    *state->qt_last_y = gy;

    send_pointer_update_event(pstate, 0);
}

static void pointer_enter(void *data, struct wl_pointer *ptr,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

#ifdef QWSWL_DEBUG_POINTER
    QWS_TRACE("surface=%p -> qws_win=%d, pos=(%d,%d)",
             (void *)surface, win->qws_id,
              wl_fixed_to_int(sx), wl_fixed_to_int(sy));
#endif

    /* zero-initialize before re-use to not have stale data 
     * produce very weird results */
    memset(&state->pointer_state, 0, sizeof(state->pointer_state));
    state->pointer_state.win = win;

    update_pointer_position(state, sx, sy);
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
                            uint32_t serial, struct wl_surface *surface)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    state->pointer_state.win = NULL;

#ifdef QWSWL_DEBUG_POINTER
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if(!win)
        return;

    QWS_TRACE("win %d", win->qws_id);
#endif
}

static void pointer_motion(void *data, struct wl_pointer *ptr,
                             uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    assert(pstate);
    qwswl_window_t *win = pstate->win;
    assert(win);

    int32_t gy = wl_fixed_to_int(sy);

    if (gy <= 35 && pstate->button_state == QWS_BTN_LEFT
            && win->xdg_toplevel) {
        /* the pointer data exists, mouse button is pressed, this is a 
         * top-level window and we're inside the region that is normally
         * associated with the window decoration, then this is likely
         * an attempt to move the window. */
        xdg_toplevel_move(win->xdg_toplevel, state->wl_seat,
            pstate->serial);

        /* Hide the event from the QWS client. We do not want it to realize
         * that a move operation is about to happen and start one on its
         * own. */
        return;
    }

    update_pointer_position(state, sx, sy);
}

static void pointer_button(void *data, struct wl_pointer *ptr,
                             uint32_t serial, uint32_t time,
                             uint32_t button, uint32_t btn_state)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    assert(pstate);
    int32_t qt_button = 0;

    /* Map Linux evdev button codes to Qt::MouseButton flags. */
    switch (button) {
    case BTN_LEFT:   qt_button = QWS_BTN_LEFT;   break;
    case BTN_RIGHT:  qt_button = QWS_BTN_RIGHT;  break;
    case BTN_MIDDLE: qt_button = QWS_BTN_MIDDLE; break;
    default:         qt_button = 0;              break;
    }

    int32_t qt_state = btn_state ? qt_button : 0;

#ifdef QWSWL_DEBUG_POINTER
    QWS_TRACE("btn=0x%x %s -> qws_win=%d qt_state=0x%x",
             button, btn_state ? "press" : "release",
             win->qws_id, qt_state);
#endif

    pstate->button_state = qt_state;
    pstate->serial = serial;

    send_pointer_update_event(pstate, 0);
}

static void pointer_axis(void *data, struct wl_pointer *ptr,
                           uint32_t time, uint32_t axis, wl_fixed_t value)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    assert(pstate);

    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    /* Wayland: positive = down; Qt delta: positive = up → negate.
     * Scale: ~10 Wayland units per notch, 120 Qt units per notch. */
    int32_t delta = -(int32_t)(wl_fixed_to_double(value) * 12.0);

#ifdef QWSWL_DEBUG_POINTER_MOVEMENT
    qwswl_window_t *win = pstate->win;
    QWS_TRACE("axis=vertical value=%.2f delta=%d -> qws_win=%d",
              wl_fixed_to_double(value), delta, win->qws_id);
#endif

    send_pointer_update_event(pstate, delta);
}

static void pointer_frame(void *data, struct wl_pointer *ptr)
{
    (void)data; (void)ptr;
}

const struct wl_pointer_listener pointer_listener = {
    .enter  = pointer_enter,
    .leave  = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis   = pointer_axis,
    .frame  = pointer_frame,
};

/* ================================================================
 * Wayland keyboard listener → QWS key events
 * ================================================================ */

static uint32_t qwswl_xkb_modifiers(struct xkb_state *state)
{
    uint32_t mods = QWS_MOD_NONE;

    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= QWS_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= QWS_MOD_CONTROL;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= QWS_MOD_ALT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= QWS_MOD_META;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= QWS_MOD_KEYPAD;
    if (xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE) != 0)
        mods |= QWS_MOD_GROUP_SWITCH;

    return mods;
}

 static void send_focus_event(qwswl_window_t *win, qws_focus_flag_t flag)
{
    qwswl_client_t *cl = win->client;
    qws_packet_t *evt = qws_make_focus_event(win->qws_id, flag);
    assert(evt);
    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void send_key_event(qwswl_keyboard_state_t *kbd_state,
                           int16_t unicode, uint32_t qt_key,
                           bool is_press, bool auto_repeat)
{
    qwswl_window_t *win = kbd_state->win;
    qwswl_client_t *cl  = win->client;

    qws_packet_t *evt = qws_make_key_event(
        win->qws_id, unicode, qt_key,
        qwswl_xkb_modifiers(kbd_state->xkb_state), is_press, auto_repeat);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void keyboard_keymap(void *data, struct wl_keyboard *kbd,
                              uint32_t format, int32_t fd, uint32_t size)
{
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map_shm != MAP_FAILED);

    kbd_state->xkb_keymap = xkb_keymap_new_from_string(
        state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(kbd_state->xkb_keymap);
    kbd_state->xkb_state = xkb_state_new(kbd_state->xkb_keymap);
    assert(kbd_state->xkb_state);

    munmap(map_shm, size);
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *kbd,
                             uint32_t serial, struct wl_surface *surface,
                             struct wl_array *keys)
{
    (void)serial;
    uint32_t *k;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

    kbd_state->win = win;

    wl_array_for_each(k, keys) {
        xkb_state_update_key(kbd_state->xkb_state, (*k) + 8, XKB_KEY_DOWN);
    }

    send_focus_event(win, QWS_FOCUS_GAIN);
}

static void keyboard_leave(void *data, struct wl_keyboard *kbd,
                              uint32_t serial, struct wl_surface *surface)
{
    (void)serial;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    kbd_state->win = NULL;

    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if (!win)
        return;

    send_focus_event(win, QWS_FOCUS_LOSE);
}

static void keyboard_key(void *data, struct wl_keyboard *kbd,
                           uint32_t serial, uint32_t time,
                           uint32_t key, uint32_t key_state)
{
    (void)serial; (void)time;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    bool is_press = key_state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t utf32 = xkb_state_key_get_utf32(kbd_state->xkb_state, key + 8);
    UChar utf16[2];

    // QWS_TRACE("evdev=%u utf32=%u %s -> qws_win=%d", key, utf32,
    //          is_press ? "press" : "release", win->qws_id);

    xkb_state_update_key(kbd_state->xkb_state, key + 8,
        is_press ? XKB_KEY_DOWN : XKB_KEY_UP);

    {
        int32_t    utf16_len = 0;
        UErrorCode err = U_ZERO_ERROR;
        UChar32    cp = (UChar32)utf32;

        u_strFromUTF32(utf16, 2, &utf16_len, &cp, 1, &err);
        assert(U_SUCCESS(err) && utf16_len == 1);
    }

    int16_t  unicode = (int16_t) utf16[0];

    send_key_event(kbd_state, unicode, key, is_press, false);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kbd,
                                 uint32_t serial,
                                 uint32_t mods_depressed,
                                 uint32_t mods_latched,
                                 uint32_t mods_locked,
                                 uint32_t group)
{
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;

    assert(kbd_state->xkb_state);

    xkb_state_update_mask(kbd_state->xkb_state, mods_depressed,
        mods_latched, mods_locked, group, group, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kbd,
                                   int32_t rate, int32_t delay)
{
    (void)data; (void)kbd; (void)rate; (void)delay;
}

const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ================================================================
 * Wayland seat listener (obtains pointer/keyboard)
 * ================================================================ */

static void seat_capabilities(void *data, struct wl_seat *seat,
                                uint32_t caps)
{
    qwswl_state_t *state = (qwswl_state_t *)data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !state->wl_pointer) {
        state->wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->wl_pointer, &pointer_listener, state);
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !state->wl_keyboard) {
        state->wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(state->wl_keyboard, &keyboard_listener, state);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};
