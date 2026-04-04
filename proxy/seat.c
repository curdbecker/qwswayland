/*
 * seat.c - QWSWayland proxy - seat event handling
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "seat.h"
#include "client.h"
#include "lifecycle.h"
#include "qws_event_factory.h"
#include "qws_trace.h"
#include "window.h"

#include <stdlib.h>
#include <string.h>
#include <unicode/ustring.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>

#include "debug.h"
#include <assert.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "stc/common.h"
#include "stc/sys/utility.h"

/* ================================================================
 * Wayland pointer listener → QWS mouse events
 * ================================================================ */

static void send_pointer_update_event(qwswl_pointer_state_t *pstate,
                                      int32_t scroll_delta) {
    qwswl_window_t *win = pstate->win;
    qwswl_client_t *cl = win->client;

    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    int32_t time = (int32_t)(_ts.tv_sec * 1000 + _ts.tv_nsec / 1000000);

    qws_packet_t *evt =
        qws_make_mouse_event(win->qws_id, pstate->gx, pstate->gy,
                             pstate->button_state, scroll_delta, time);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void update_pointer_position(qwswl_state_t *state, wl_fixed_t sx,
                                    wl_fixed_t sy) {
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    qwswl_window_t *win = pstate->win;
    int32_t gx, gy;

    /* Translate surface-local coords to QWS global coords */
    gx = win->geometry.x + wl_fixed_to_int(sx);
    gy = win->geometry.y + wl_fixed_to_int(sy);

#ifdef QWSWL_DEBUG_POINTER_MOVEMENT
    QWS_TRACE("qws_win=%d, surface_local=(%d,%d), global=(%d,%d)", win->qws_id,
              wl_fixed_to_int(sx), wl_fixed_to_int(sy), gx, gy);
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

static void pointer_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx,
                          wl_fixed_t sy) {
    (void)ptr;
    (void)serial;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    assert(win);

#ifdef QWSWL_DEBUG_POINTER
    QWS_TRACE("surface=%p -> qws_win=%d, pos=(%d,%d)", (void *)surface,
              win->qws_id, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
#endif

    /* zero-initialize before re-use to not have stale data
     * produce very weird results */
    memset(&state->pointer_state, 0, sizeof(state->pointer_state));
    state->pointer_state.win = win;

    update_pointer_position(state, sx, sy);
}

static void pointer_leave(void *data, struct wl_pointer *ptr, uint32_t serial,
                          struct wl_surface *surface) {
    (void)ptr;
    (void)serial;
    (void)surface;
    qwswl_state_t *state = (qwswl_state_t *)data;
    state->pointer_state.win = NULL;

#ifdef QWSWL_DEBUG_POINTER
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if (!win)
        return;

    QWS_TRACE("win %d", win->qws_id);
#endif
}

static void pointer_motion(void *data, struct wl_pointer *ptr, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr;
    (void)time;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    assert(pstate);
    qwswl_window_t *win = pstate->win;
    assert(win);

    int32_t gy = wl_fixed_to_int(sy);

    if (gy <= 35 && pstate->button_state == QWS_BTN_LEFT && win->xdg_toplevel) {
        /* the pointer data exists, mouse button is pressed, this is a
         * top-level window and we're inside the region that is normally
         * associated with the window decoration, then this is likely
         * an attempt to move the window. */
        xdg_toplevel_move(win->xdg_toplevel, state->wl_seat, pstate->serial);

        /* Hide the event from the QWS client. We do not want it to realize
         * that a move operation is about to happen and start one on its
         * own. */
        return;
    }

    update_pointer_position(state, sx, sy);
}

static void pointer_button(void *data, struct wl_pointer *ptr, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t btn_state) {
    (void)ptr;
    (void)time;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_pointer_state_t *pstate = &state->pointer_state;
    assert(pstate);
    int32_t qt_button = 0;

    /* Map Linux evdev button codes to Qt::MouseButton flags. */
    switch (button) {
    case BTN_LEFT:
        qt_button = QWS_BTN_LEFT;
        break;
    case BTN_RIGHT:
        qt_button = QWS_BTN_RIGHT;
        break;
    case BTN_MIDDLE:
        qt_button = QWS_BTN_MIDDLE;
        break;
    default:
        qt_button = 0;
        break;
    }

    int32_t qt_state = btn_state ? qt_button : 0;

#ifdef QWSWL_DEBUG_POINTER
    QWS_TRACE("btn=0x%x %s -> qws_win=%d qt_state=0x%x", button,
              btn_state ? "press" : "release", win->qws_id, qt_state);
#endif

    if (btn_state && pstate->win)
        qwswl_window_set_focus(state, pstate->win, true, true);

    pstate->button_state = qt_state;
    pstate->serial = serial;
    state->last_input_serial = serial;

    send_pointer_update_event(pstate, 0);
}

static void pointer_axis(void *data, struct wl_pointer *ptr, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)ptr;
    (void)time;
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

static void pointer_frame(void *data, struct wl_pointer *ptr) {
    (void)data;
    (void)ptr;
}

const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
};

/* ================================================================
 * Wayland keyboard listener → QWS key events
 * ================================================================ */

/* Translate an XKB keysym to the Qt::Key value expected by QWS.
 *
 * XKB keysyms and Qt::Key are not the same numbering scheme.  For printable
 * characters Qt uses the (uppercase) Unicode codepoint; for special keys it
 * uses an opaque 0x01xxxxxx range.  The lookup table below covers the special-
 * key cases; arithmetic shortcuts handle F-keys and KP digits; xkbcommon's
 * own xkb_keysym_to_utf32 / xkb_keysym_to_upper handle everything else.
 *
 * Derived from Qt 4.8 KeyTbl[] in qkeymapper_x11.cpp. */
static uint32_t keysym_to_qt_key(xkb_keysym_t sym) {
    /* Pairs of { xkb_keysym, Qt::Key }. */
    static const uint32_t keytbl[] = {
        /* misc */
        XKB_KEY_Escape, 0x01000000u,       /* Qt::Key_Escape */
        XKB_KEY_Tab, 0x01000001u,          /* Qt::Key_Tab */
        XKB_KEY_ISO_Left_Tab, 0x01000002u, /* Qt::Key_Backtab */
        XKB_KEY_BackSpace, 0x01000003u,    /* Qt::Key_Backspace */
        XKB_KEY_Return, 0x01000004u,       /* Qt::Key_Return */
        XKB_KEY_KP_Enter, 0x01000005u,     /* Qt::Key_Enter */
        XKB_KEY_Insert, 0x01000006u,       /* Qt::Key_Insert */
        XKB_KEY_Delete, 0x01000007u,       /* Qt::Key_Delete */
        XKB_KEY_Pause, 0x01000008u,        /* Qt::Key_Pause */
        XKB_KEY_Print, 0x01000009u,        /* Qt::Key_Print */
        XKB_KEY_Sys_Req, 0x0100000au,      /* Qt::Key_SysReq */
        XKB_KEY_Clear, 0x0100000bu,        /* Qt::Key_Clear */
        /* cursor movement */
        XKB_KEY_Home, 0x01000010u,  /* Qt::Key_Home */
        XKB_KEY_End, 0x01000011u,   /* Qt::Key_End */
        XKB_KEY_Left, 0x01000012u,  /* Qt::Key_Left */
        XKB_KEY_Up, 0x01000013u,    /* Qt::Key_Up */
        XKB_KEY_Right, 0x01000014u, /* Qt::Key_Right */
        XKB_KEY_Down, 0x01000015u,  /* Qt::Key_Down */
        XKB_KEY_Prior, 0x01000016u, /* Qt::Key_PageUp */
        XKB_KEY_Next, 0x01000017u,  /* Qt::Key_PageDown */
        /* modifiers */
        XKB_KEY_Shift_L, 0x01000020u, /* Qt::Key_Shift */
        XKB_KEY_Shift_R, 0x01000020u, XKB_KEY_Shift_Lock, 0x01000020u,
        XKB_KEY_Control_L, 0x01000021u, /* Qt::Key_Control */
        XKB_KEY_Control_R, 0x01000021u, XKB_KEY_Meta_L,
        0x01000022u, /* Qt::Key_Meta */
        XKB_KEY_Meta_R, 0x01000022u, XKB_KEY_Alt_L,
        0x01000023u, /* Qt::Key_Alt */
        XKB_KEY_Alt_R, 0x01000023u, XKB_KEY_Caps_Lock,
        0x01000024u,                           /* Qt::Key_CapsLock */
        XKB_KEY_Num_Lock, 0x01000025u,         /* Qt::Key_NumLock */
        XKB_KEY_Scroll_Lock, 0x01000026u,      /* Qt::Key_ScrollLock */
        XKB_KEY_Super_L, 0x01000053u,          /* Qt::Key_Super_L */
        XKB_KEY_Super_R, 0x01000054u,          /* Qt::Key_Super_R */
        XKB_KEY_Menu, 0x01000055u,             /* Qt::Key_Menu */
        XKB_KEY_Hyper_L, 0x01000056u,          /* Qt::Key_Hyper_L */
        XKB_KEY_Hyper_R, 0x01000057u,          /* Qt::Key_Hyper_R */
        XKB_KEY_Help, 0x01000058u,             /* Qt::Key_Help */
        XKB_KEY_ISO_Level3_Shift, 0x01001103u, /* Qt::Key_AltGr */
        XKB_KEY_Multi_key, 0x01001120u,        /* Qt::Key_Multi_key */
        XKB_KEY_Mode_switch, 0x0100117eu,      /* Qt::Key_Mode_switch */
        /* KP navigation */
        XKB_KEY_KP_Space, 0x00000020u,     /* Qt::Key_Space */
        XKB_KEY_KP_Tab, 0x01000001u,       /* Qt::Key_Tab */
        XKB_KEY_KP_Home, 0x01000010u,      /* Qt::Key_Home */
        XKB_KEY_KP_Left, 0x01000012u,      /* Qt::Key_Left */
        XKB_KEY_KP_Up, 0x01000013u,        /* Qt::Key_Up */
        XKB_KEY_KP_Right, 0x01000014u,     /* Qt::Key_Right */
        XKB_KEY_KP_Down, 0x01000015u,      /* Qt::Key_Down */
        XKB_KEY_KP_Prior, 0x01000016u,     /* Qt::Key_PageUp */
        XKB_KEY_KP_Next, 0x01000017u,      /* Qt::Key_PageDown */
        XKB_KEY_KP_End, 0x01000011u,       /* Qt::Key_End */
        XKB_KEY_KP_Begin, 0x0100000bu,     /* Qt::Key_Clear */
        XKB_KEY_KP_Insert, 0x01000006u,    /* Qt::Key_Insert */
        XKB_KEY_KP_Delete, 0x01000007u,    /* Qt::Key_Delete */
        XKB_KEY_KP_Equal, 0x0000003du,     /* Qt::Key_Equal */
        XKB_KEY_KP_Multiply, 0x0000002au,  /* Qt::Key_Asterisk */
        XKB_KEY_KP_Add, 0x0000002bu,       /* Qt::Key_Plus */
        XKB_KEY_KP_Separator, 0x0000002cu, /* Qt::Key_Comma */
        XKB_KEY_KP_Subtract, 0x0000002du,  /* Qt::Key_Minus */
        XKB_KEY_KP_Decimal, 0x0000002eu,   /* Qt::Key_Period */
        XKB_KEY_KP_Divide, 0x0000002fu,    /* Qt::Key_Slash */
        /* dead (compose) keys */
        XKB_KEY_dead_grave, 0x01001250u,        /* Qt::Key_Dead_Grave */
        XKB_KEY_dead_acute, 0x01001251u,        /* Qt::Key_Dead_Acute */
        XKB_KEY_dead_circumflex, 0x01001252u,   /* Qt::Key_Dead_Circumflex */
        XKB_KEY_dead_tilde, 0x01001253u,        /* Qt::Key_Dead_Tilde */
        XKB_KEY_dead_macron, 0x01001254u,       /* Qt::Key_Dead_Macron */
        XKB_KEY_dead_breve, 0x01001255u,        /* Qt::Key_Dead_Breve */
        XKB_KEY_dead_abovedot, 0x01001256u,     /* Qt::Key_Dead_Abovedot */
        XKB_KEY_dead_diaeresis, 0x01001257u,    /* Qt::Key_Dead_Diaeresis */
        XKB_KEY_dead_abovering, 0x01001258u,    /* Qt::Key_Dead_Abovering */
        XKB_KEY_dead_doubleacute, 0x01001259u,  /* Qt::Key_Dead_Doubleacute */
        XKB_KEY_dead_caron, 0x0100125au,        /* Qt::Key_Dead_Caron */
        XKB_KEY_dead_cedilla, 0x0100125bu,      /* Qt::Key_Dead_Cedilla */
        XKB_KEY_dead_ogonek, 0x0100125cu,       /* Qt::Key_Dead_Ogonek */
        XKB_KEY_dead_iota, 0x0100125du,         /* Qt::Key_Dead_Iota */
        XKB_KEY_dead_voiced_sound, 0x0100125eu, /* Qt::Key_Dead_Voiced_Sound */
        XKB_KEY_dead_semivoiced_sound,
        0x0100125fu,                        /* Qt::Key_Dead_Semivoiced_Sound */
        XKB_KEY_dead_belowdot, 0x01001260u, /* Qt::Key_Dead_Belowdot */
        XKB_KEY_dead_hook, 0x01001261u,     /* Qt::Key_Dead_Hook */
        XKB_KEY_dead_horn, 0x01001262u,     /* Qt::Key_Dead_Horn */
        /* multimedia / browser */
        XKB_KEY_XF86Back, 0x01000061u,             /* Qt::Key_Back */
        XKB_KEY_XF86Forward, 0x01000062u,          /* Qt::Key_Forward */
        XKB_KEY_XF86Stop, 0x01000063u,             /* Qt::Key_Stop */
        XKB_KEY_XF86Refresh, 0x01000064u,          /* Qt::Key_Refresh */
        XKB_KEY_XF86Favorites, 0x01000091u,        /* Qt::Key_Favorites */
        XKB_KEY_XF86AudioMedia, 0x010000a1u,       /* Qt::Key_LaunchMedia */
        XKB_KEY_XF86HomePage, 0x01000090u,         /* Qt::Key_HomePage */
        XKB_KEY_XF86Search, 0x01000092u,           /* Qt::Key_Search */
        XKB_KEY_XF86AudioLowerVolume, 0x01000070u, /* Qt::Key_VolumeDown */
        XKB_KEY_XF86AudioMute, 0x01000071u,        /* Qt::Key_VolumeMute */
        XKB_KEY_XF86AudioRaiseVolume, 0x01000072u, /* Qt::Key_VolumeUp */
        XKB_KEY_XF86AudioPlay, 0x01000080u,        /* Qt::Key_MediaPlay */
        XKB_KEY_XF86AudioStop, 0x01000081u,        /* Qt::Key_MediaStop */
        XKB_KEY_XF86AudioPrev, 0x01000082u,        /* Qt::Key_MediaPrevious */
        XKB_KEY_XF86AudioNext, 0x01000083u,        /* Qt::Key_MediaNext */
        XKB_KEY_XF86AudioRecord, 0x01000084u,      /* Qt::Key_MediaRecord */
        XKB_KEY_XF86Mail, 0x010000a0u,             /* Qt::Key_LaunchMail */
        0u, 0u};

    /* F-keys: XKB_KEY_F1 (0xffbe) .. XKB_KEY_F35 (0xffe0) → Qt::Key_F1 + offset
     */
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35)
        return 0x01000030u + (sym - XKB_KEY_F1);

    /* KP digit keys: XKB_KEY_KP_0 (0xffb0) .. XKB_KEY_KP_9 (0xffb9) */
    if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9)
        return 0x00000030u + (sym - XKB_KEY_KP_0);

    for (size_t i = 0; keytbl[i]; i += 2)
        if (keytbl[i] == (uint32_t)sym)
            return keytbl[i + 1];

    /* For all remaining keysyms, xkbcommon can convert to a Unicode codepoint.
     * Qt::Key for letter keys is the uppercase codepoint, so normalize first.
     */
    uint32_t ucs = xkb_keysym_to_utf32(xkb_keysym_to_upper(sym));
    if (ucs)
        return ucs;

    return 0x01ffffffu; /* Qt::Key_unknown */
}

static uint32_t qwswl_xkb_modifiers(struct xkb_state *state) {
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


static void kbd_repeat_disarm(qwswl_keyboard_state_t *kbd_state) {
    if (kbd_state->repeat_timerfd < 0)
        return;
    struct itimerspec ts = {0};
    timerfd_settime(kbd_state->repeat_timerfd, 0, &ts, NULL);
    kbd_state->repeat_key = 0;
}

static void send_key_event(qwswl_keyboard_state_t *kbd_state, int16_t unicode,
                           uint32_t qt_key, bool is_press, bool auto_repeat) {
    qwswl_window_t *win = kbd_state->win;
    qwswl_client_t *cl = win->client;

    qws_packet_t *evt = qws_make_key_event(
        win->qws_id, unicode, qt_key, qwswl_xkb_modifiers(kbd_state->xkb_state),
        is_press, auto_repeat);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void keyboard_keymap(void *data, struct wl_keyboard *kbd,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)kbd;
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

static void keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)kbd;
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

    qwswl_window_set_focus(state, win, true, true);
}

static void keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                           struct wl_surface *surface) {
    (void)kbd;
    (void)serial;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    kbd_state->win = NULL;
    kbd_repeat_disarm(kbd_state);

    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if (!win)
        return;

    qwswl_window_set_focus(state, win, false, true);
}

static void keyboard_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t key_state) {
    (void)kbd;
    (void)time;
    qwswl_state_t *state = (qwswl_state_t *)data;
    state->last_input_serial = serial;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    bool is_press = key_state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t utf32 = xkb_state_key_get_utf32(kbd_state->xkb_state, key + 8);
    xkb_keysym_t keysym =
        xkb_state_key_get_one_sym(kbd_state->xkb_state, key + 8);
    UChar utf16[2];

#ifdef DEBUG_KBD_KEY
    qwswl_window_t *win = kbd_state->win;
    QWS_TRACE("evdev=%u utf32=%u %s -> qws_win=%d", key, utf32,
              is_press ? "press" : "release", win->qws_id);
#endif

    xkb_state_update_key(kbd_state->xkb_state, key + 8,
                         is_press ? XKB_KEY_DOWN : XKB_KEY_UP);

    {
        int32_t utf16_len = 0;
        UErrorCode err = U_ZERO_ERROR;
        UChar32 cp = (UChar32)utf32;

        u_strFromUTF32(utf16, 2, &utf16_len, &cp, 1, &err);
        assert(U_SUCCESS(err) && utf16_len == 1);
    }

    int16_t unicode = (int16_t)utf16[0];
    uint32_t qt_key = keysym_to_qt_key(keysym);

    send_key_event(kbd_state, unicode, qt_key, is_press, false);

    /* Arm or disarm the repeat timer. */
    if (!is_press) {
        if (key == kbd_state->repeat_key)
            kbd_repeat_disarm(kbd_state);
    } else if (kbd_state->repeat_timerfd >= 0 && kbd_state->repeat_rate > 0 &&
               xkb_keymap_key_repeats(kbd_state->xkb_keymap, key + 8)) {
        int32_t interval_ms = 1000 / kbd_state->repeat_rate;
        struct itimerspec ts = {
            .it_value = {.tv_sec = kbd_state->repeat_delay / 1000,
                         .tv_nsec =
                             (kbd_state->repeat_delay % 1000) * 1000000L},
            .it_interval = {.tv_sec = interval_ms / 1000,
                            .tv_nsec = (interval_ms % 1000) * 1000000L},
        };
        kbd_state->repeat_key = key;
        kbd_state->repeat_unicode = unicode;
        kbd_state->repeat_qt_key = qt_key;
        timerfd_settime(kbd_state->repeat_timerfd, 0, &ts, NULL);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kbd,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
    (void)kbd;
    (void)serial;
    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;

    assert(kbd_state->xkb_state);

    xkb_state_update_mask(kbd_state->xkb_state, mods_depressed, mods_latched,
                          mods_locked, group, group, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kbd,
                                 int32_t rate, int32_t delay) {
    (void)kbd;
    qwswl_state_t *state = (qwswl_state_t *)data;
    state->kbd_state.repeat_rate = rate;
    state->kbd_state.repeat_delay = delay;
}

void qwswl_keyboard_repeat_tick(qwswl_state_t *state) {
    qwswl_keyboard_state_t *kbd_state = &state->kbd_state;
    uint64_t expirations;

    /* Drain the timerfd — required even when we have nothing to send,
     * otherwise epoll will keep reporting it as ready. */
    read(kbd_state->repeat_timerfd, &expirations, sizeof(expirations));

    if (!kbd_state->win)
        return;

    send_key_event(kbd_state, kbd_state->repeat_unicode,
                   kbd_state->repeat_qt_key, true, true);
}

const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ================================================================
 * Wayland seat listener (obtains pointer/keyboard)
 * ================================================================ */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
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

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};
