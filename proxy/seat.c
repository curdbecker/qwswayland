/*
 * seat.c - QWSWayland proxy - seat event handling 
 * SPDX-License-Identifier: MIT
 */

#include "seat.h"
#include "window.h"
#include "lifecycle.h"
#include "client.h"
#include "qws_server_helpers.h"
#include "qws_trace.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unicode/ustring.h>

#include <time.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>

#include <assert.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "stc/common.h"
#include "stc/sys/utility.h"

extern qwswl_state_t g_state;

/* ================================================================
 * Wayland pointer listener → QWS mouse events
 * ================================================================ */

static void update_pointer_position(qwswl_pointer_data_t *pointer_data,
                                    wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_window_t *win = pointer_data->win;
    assert(pointer_data && win);

    qwswl_client_t *cl = win->client;

    /* Translate surface-local coords to QWS global coords, but 
     * clamp them to the maximum possible screen coordinates. 
     * This should prevent the QWS client to attempt moving 
     * elements out of the window surface. */
    pointer_data->gx = 
        c_min(c_max(win->geometry.x + wl_fixed_to_int(sx), 0), 
            g_state.screen_width);
    pointer_data->gy = 
        c_min(c_max(win->geometry.y + wl_fixed_to_int(sy), 0),
            g_state.screen_height);

#ifdef QWSWL_DEBUG_POINTER_MOVEMENT
    QWS_TRACE("qws_win=%d, surface_local=(%d,%d), global=(%d,%d)",
             win->qws_id,
             wl_fixed_to_int(sx), wl_fixed_to_int(sy),
             pointer_data->gx, pointer_data->gy);
#endif

    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    int32_t time = (int32_t)(_ts.tv_sec * 1000 + _ts.tv_nsec / 1000000);

    /* Send QWS mouse event to the owning client */
    qws_packet_t *evt = qws_make_mouse_event(
        win->qws_id, pointer_data->gx, pointer_data->gy,
        pointer_data->button_state, 0, time);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void pointer_enter(void *data, struct wl_pointer *ptr,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

    /* zero-initialize to not have random (or stale during reallocation)
     * leak into the new pointer context and produce very weird results */
    qwswl_pointer_data_t *pointer_data =
        calloc(1, sizeof(qwswl_pointer_data_t));
    assert(pointer_data);

    pointer_data->win = win;
    wl_pointer_set_user_data(ptr, (void *) pointer_data);

    update_pointer_position(pointer_data, sx, sy);

#ifdef QWSWL_DEBUG_POINTER
    QWS_TRACE("surface=%p -> qws_win=%d, pos=(%d,%d)",
             (void *)surface, win->qws_id,
              wl_fixed_to_int(sx), wl_fixed_to_int(sy));
#endif
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
                            uint32_t serial, struct wl_surface *surface)
{
    qwswl_pointer_data_t *pointer_data = wl_pointer_get_user_data(ptr);
    free(pointer_data);
    wl_pointer_set_user_data(ptr, NULL);

#ifdef QWSWL_DEBUG_POINTER
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

    QWS_TRACE("win %d", win->qws_id);
#endif
}

static void pointer_motion(void *data, struct wl_pointer *ptr,
                             uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_pointer_data_t *pointer_data =
        (qwswl_pointer_data_t *) wl_pointer_get_user_data(ptr);
    assert(pointer_data);
    qwswl_window_t *win = pointer_data->win;
    assert(win);

    int32_t gy = wl_fixed_to_int(sy);

    if (gy <= 35 && pointer_data->button_state == QWS_BTN_LEFT 
            && win->xdg_toplevel) {
        /* the pointer data exists, mouse button is pressed, this is a 
         * top-level window and we're inside the region that is normally
         * associated with the window decoration, then this is likely
         * an attempt to move the window. */
        xdg_toplevel_move(win->xdg_toplevel, g_state.wl_seat,
            pointer_data->serial);

        /* Hide the event from the QWS client. We do not want it to realize
         * that a move operation is about to happen and start one on its
         * own. */
        return;
    }

    update_pointer_position(pointer_data, sx, sy);
}

static void pointer_button(void *data, struct wl_pointer *ptr,
                             uint32_t serial, uint32_t time,
                             uint32_t button, uint32_t btn_state)
{
    qwswl_pointer_data_t *pointer_data =
        (qwswl_pointer_data_t *) wl_pointer_get_user_data(ptr);
    qwswl_window_t *win = pointer_data->win;
    qwswl_client_t *cl = win->client;
    int32_t qt_button = 0;

    assert(pointer_data && win);

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

    pointer_data->button_state = qt_state;
    pointer_data->serial = serial;

    qws_packet_t *evt = qws_make_mouse_event(
        win->qws_id, pointer_data->gx, pointer_data->gy, qt_state, 0, (int32_t)time);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);

    qws_packet_free(evt);
}

static void pointer_axis(void *data, struct wl_pointer *ptr,
                           uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    qwswl_pointer_data_t *pointer_data = wl_pointer_get_user_data(ptr);
    if (!pointer_data)
        return;

    qwswl_window_t *win = pointer_data->win;
    qwswl_client_t *cl = win->client;

    /* Wayland: positive = down; Qt delta: positive = up → negate.
     * Scale: ~10 Wayland units per notch, 120 Qt units per notch. */
    int32_t delta = -(int32_t)(wl_fixed_to_double(value) * 12.0);

#ifdef QWSWL_DEBUG_POINTER_MOVEMENT
    QWS_TRACE("axis=vertical value=%.2f delta=%d -> qws_win=%d",
              wl_fixed_to_double(value), delta, win->qws_id);
#endif

    qws_packet_t *evt = qws_make_mouse_event(
        win->qws_id, pointer_data->gx, pointer_data->gy,
        0, delta, (int32_t)time);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
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

static void keyboard_keymap(void *data, struct wl_keyboard *kbd,
                              uint32_t format, int32_t fd, uint32_t size)
{
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_data_t *kbd_data = malloc(sizeof(*kbd_data));
    assert(kbd_data != NULL);

    wl_keyboard_set_user_data(kbd, kbd_data);

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map_shm != MAP_FAILED);

    kbd_data->xkb_keymap = xkb_keymap_new_from_string(
        state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(kbd_data->xkb_keymap);
    kbd_data->xkb_state = xkb_state_new(kbd_data->xkb_keymap);
    assert(kbd_data->xkb_state);

    munmap(map_shm, size);
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *kbd,
                             uint32_t serial, struct wl_surface *surface,
                             struct wl_array *keys)
{
    (void)data; (void)serial;
    uint32_t *k;
    qwswl_keyboard_data_t *kbd_data = wl_keyboard_get_user_data(kbd);
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

    kbd_data->win = win;

    // QWS_TRACE("qws_win=%d", win->qws_id);

    wl_array_for_each(k, keys) {
        xkb_state_update_key(kbd_data->xkb_state, (*k) + 8, XKB_KEY_DOWN);
    }
    
    qwswl_client_t *cl = win->client;
    qws_packet_t *evt = qws_make_focus_event(win->qws_id, QWS_FOCUS_GAIN);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void keyboard_leave(void *data, struct wl_keyboard *kbd,
                              uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)serial;
    qwswl_keyboard_data_t *kbd_data = wl_keyboard_get_user_data(kbd);
    qwswl_window_t *win = kbd_data->win;
    assert(kbd_data && win);

    // QWS_TRACE("qws_win=%d", win->qws_id);

    qwswl_client_t *cl = win->client;
    qws_packet_t *evt = qws_make_focus_event(win->qws_id, QWS_FOCUS_LOSE);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);

    kbd_data->win = NULL;
}

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

static void keyboard_key(void *data, struct wl_keyboard *kbd,
                           uint32_t serial, uint32_t time,
                           uint32_t key, uint32_t key_state)
{
    (void)serial; (void)time;
    qwswl_keyboard_data_t *kbd_data = wl_keyboard_get_user_data(kbd);
    qwswl_window_t *win = kbd_data->win;
    qwswl_client_t *cl = win->client;
    bool is_press = key_state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t utf32 = xkb_state_key_get_utf32(kbd_data->xkb_state, key + 8);
    UChar utf16[2];

    // QWS_TRACE("evdev=%u utf32=%u %s -> qws_win=%d", key, utf32,
    //          is_press ? "press" : "release", win->qws_id);

    xkb_state_update_key(kbd_data->xkb_state, key + 8,
        is_press ? XKB_KEY_DOWN : XKB_KEY_UP);

    {
        int32_t    utf16_len = 0;
        UErrorCode err = U_ZERO_ERROR;
        UChar32    cp = (UChar32)utf32;

        u_strFromUTF32(utf16, 2, &utf16_len, &cp, 1, &err);
        assert(U_SUCCESS(err) && utf16_len == 1);
    }

    qws_packet_t *evt = qws_make_key_event(
        win->qws_id, (int16_t) utf16[0], key,
        qwswl_xkb_modifiers(kbd_data->xkb_state), is_press, false);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kbd,
                                 uint32_t serial,
                                 uint32_t mods_depressed,
                                 uint32_t mods_latched,
                                 uint32_t mods_locked,
                                 uint32_t group)
{
    qwswl_keyboard_data_t *kbd_data = wl_keyboard_get_user_data(kbd);

    assert(kbd_data->xkb_state);

    xkb_state_update_mask(kbd_data->xkb_state, mods_depressed,
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
