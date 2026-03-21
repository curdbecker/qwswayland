/*
 * qwswayland.c - QWSWayland proxy daemon implementation
 * SPDX-License-Identifier: MIT
 */

#include "qwswayland.h"
#include "qws_server_helpers.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unicode/ustring.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <fcntl.h>

#include <assert.h>


static qwswl_window_t *qwswl_find_in_use_window(qwswl_state_t *state, int32_t qws_id);

/* -----------------------------------------------------------
 * Trace helpers: macro for Wayland-side event logging
 * ----------------------------------------------------------- */

/* Log a Wayland-side event at verbose >= 2 */
#define WL_TRACE(fmt, ...) \
    do { \
        fprintf(stderr, "[wl] %s: " fmt "\n", __func__, ##__VA_ARGS__); \
    } while (0)

#define min(a, b) ({            \
    __typeof__(a) _a = (a);     \
    __typeof__(b) _b = (b);     \
    _a < _b ? _a : _b;          \
})

/* Resolve a Wayland surface to its QWS window.
 * Returns the window only if surface is non-NULL and the window is in use.
 * Callers should return immediately when this returns NULL. */
static inline qwswl_window_t *
qwswl_surface_to_win(struct wl_surface *surface)
{
    if (!surface)
        return NULL;
    qwswl_window_t *win = (qwswl_window_t *)wl_surface_get_user_data(surface);
    if (!win || !win->in_use)
        return NULL;
    return win;
}

/* ================================================================
 * Wayland registry listener
 * ================================================================ */

static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *interface,
                             uint32_t version)

{
    qwswl_state_t *state = (qwswl_state_t *)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        state->wl_compositor = wl_registry_bind(reg, name,
                                                 &wl_compositor_interface, version);
        WL_TRACE("registry: found wl_compositor (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_shm") == 0) {
        state->wl_shm = wl_registry_bind(reg, name,
                                           &wl_shm_interface, 1);
        WL_TRACE("registry: found wl_shm (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_seat") == 0) {
        state->wl_seat = wl_registry_bind(reg, name,
                                            &wl_seat_interface, version);
        WL_TRACE("registry: found wl_seat (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_output") == 0) {
        state->wl_output = wl_registry_bind(reg, name,
                                              &wl_output_interface, version);
        WL_TRACE("registry: found wl_output (name=%u, max_versionv=%u)", name, version);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        state->xdg_wm_base = wl_registry_bind(reg, name,
                                               &xdg_wm_base_interface, version);
        WL_TRACE("registry: found xdg_wm_base (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "zxdg_output_manager_v1") == 0) {
        state->xdg_output_manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, version);
        WL_TRACE("registry: found zxdg_output_manager_v1 (name=%u, max_version=%u)", name, version);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        state->wl_subcompositor = wl_registry_bind(reg, name,
                                                    &wl_subcompositor_interface, 1);
        WL_TRACE("registry: found wl_subcompositor (name=%u, max_version=%u)", name, version);
    } else {
        WL_TRACE("registry: skipped %s (name=%u, v=%u)", interface, name, version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                    uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

typedef struct qwswl_pointer_data {
    qwswl_window_t *win;
    int32_t gx;
    int32_t gy;
} qwswl_pointer_data_t;

/* ================================================================
 * Wayland pointer listener → QWS mouse events
 * ================================================================ */

static void update_pointer_position(qwswl_pointer_data_t *pointer_data,
                                    wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_window_t *win = pointer_data->win;
    assert(pointer_data && win);

    qwswl_client_t *cl = win->client;

    if (!win->in_use) {
        WL_TRACE("window is not in use anymore! destroyed?");
        return;
    }

    /* Translate surface-local coords to QWS global coords */
    pointer_data->gx = win->geometry.x + wl_fixed_to_int(sx);
    pointer_data->gy = win->geometry.y + wl_fixed_to_int(sy);

    if (win->parent) {
        qwswl_window_t *parent = win->parent;
        /* for child windows in a sub-surface the position is
         * again given relative to the parent window, so we need
         * again to compensate when calculating the global offset */
        pointer_data->gx += parent->geometry.x;
        pointer_data->gy += parent->geometry.y;
    }

    WL_TRACE("qws_win=%d, surface_local=(%d,%d), global=(%d,%d)",
             win->qws_id,
             wl_fixed_to_int(sx), wl_fixed_to_int(sy),
             pointer_data->gx, pointer_data->gy);

    // assert(pointer_data->gx >= 0 && pointer_data->gy >= 0);

    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    int32_t time = (int32_t)(_ts.tv_sec * 1000 + _ts.tv_nsec / 1000000);

    /* Send QWS mouse event to the owning client */
    qws_packet_t *evt = qws_make_mouse_event(
        win->qws_id, pointer_data->gx, pointer_data->gy,
        0, 0, time);
    
    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void pointer_enter(void *data, struct wl_pointer *ptr,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if (!win) {
        WL_TRACE("surface or window not available");
        return;
    }

    qwswl_state_t *state = (qwswl_state_t *)data;
    qwswl_client_t *cl = win->client;
    qwswl_pointer_data_t *pointer_data =
        malloc(sizeof(qwswl_pointer_data_t));
    assert(pointer_data);

    pointer_data->win = win;
    wl_pointer_set_user_data(ptr, (void *) pointer_data);

    update_pointer_position(pointer_data, sx, sy);

    WL_TRACE("surface=%p -> qws_win=%d, pos=(%d,%d)",
             (void *)surface, win->qws_id,
              wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
                            uint32_t serial, struct wl_surface *surface)
{
    qwswl_pointer_data_t *pointer_data = wl_pointer_get_user_data(ptr);
    free(pointer_data);
    wl_pointer_set_user_data(ptr, NULL);

    qwswl_window_t *win = qwswl_surface_to_win(surface);
    if (win)
        WL_TRACE("left qws_id=%d", win->qws_id);
    else
        WL_TRACE("left destroyed window");
}

static void pointer_motion(void *data, struct wl_pointer *ptr,
                             uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    qwswl_pointer_data_t *pointer_data = 
        (qwswl_pointer_data_t *) wl_pointer_get_user_data(ptr);

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

    if (!win->in_use) {
        WL_TRACE("window is not in use anymore! destroyed?");
        return;
    }

    /*
     * Map Linux button codes to Qt::MouseButton flags:
     *   BTN_LEFT   (0x110) → Qt::LeftButton   (0x01)
     *   BTN_RIGHT  (0x111) → Qt::RightButton  (0x02)
     *   BTN_MIDDLE (0x112) → Qt::MiddleButton  (0x04)
     */
    switch (button) {
    case 0x110: qt_button = 0x01; break;  /* Left */
    case 0x111: qt_button = 0x02; break;  /* Right */
    case 0x112: qt_button = 0x04; break;  /* Middle */
    default:    qt_button = 0;    break;
    }

    int32_t qt_state = btn_state ? qt_button : 0;

    WL_TRACE("btn=0x%x %s -> qws_win=%d qt_state=0x%x",
             button, btn_state ? "press" : "release",
             win->qws_id, qt_state);

    // assert(pointer_data->gx >= 0 && pointer_data->gy >= 0);

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
    /* TODO: translate scroll to QWS wheel events */
    (void)data; (void)ptr; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void* data, struct wl_pointer* ptr)
{
}


static const struct wl_pointer_listener pointer_listener = {
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

typedef struct qwswl_keyboard_data {
    struct xkb_keymap   *xkb_keymap;
    struct xkb_state    *xkb_state;
    qwswl_window_t      *win;
} qwswl_keyboard_data_t;

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
    if (!win) {
        WL_TRACE("surface or window not available");
        return;
    }

    win->focused = true;
    kbd_data->win = win;

    WL_TRACE("qws_win=%d", win->qws_id);

    wl_array_for_each(k, keys) {
        xkb_state_update_key(kbd_data->xkb_state, (*k) + 8, XKB_KEY_DOWN);
    }

    qwswl_client_t *cl = win->client;
    qws_packet_t *evt = qws_make_focus_event(win->qws_id, 1);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);
}

static void keyboard_leave(void *data, struct wl_keyboard *kbd,
                              uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)serial; (void)surface;
    qwswl_keyboard_data_t *kbd_data = wl_keyboard_get_user_data(kbd);
    qwswl_window_t *win = kbd_data->win;
    assert(kbd_data && win);

    if (!win->in_use)
        return;

    WL_TRACE("qws_win=%d", win->qws_id);

    qwswl_client_t *cl = win->client;
    qws_packet_t *evt = qws_make_focus_event(win->qws_id, 0);
    assert(evt);

    qws_trace_packet(cl->client_id, evt, true);
    qws_write_packet(cl->fd, evt);
    qws_packet_free(evt);

    win->focused = false;
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

    WL_TRACE("evdev=%u utf32=%u %s -> qws_win=%d", key, utf32,
             is_press ? "press" : "release", win->qws_id);

    xkb_state_update_key(kbd_data->xkb_state, key + 8, 
        is_press ? XKB_KEY_DOWN : XKB_KEY_UP);

    if (utf32 == 0) {
        return;
    }

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

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ================================================================
 * Wayland seat listener (to get pointer/keyboard)
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

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void xdg_base_wm_ping(void *data, struct xdg_wm_base *xdg_wm_base,
				uint32_t serial) {
	(void) data;
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	xdg_base_wm_ping
};

/* ================================================================
 * xdg_output listener
 * ================================================================ */

static void xdg_output_logical_position(void *data,
                                        struct zxdg_output_v1 *output,
                                        int32_t x, int32_t y)
{
    (void)data; (void)output;
    WL_TRACE("xdg_output: logical_position %d,%d", x, y);
}

static void xdg_output_logical_size(void *data,
                                    struct zxdg_output_v1 *output,
                                    int32_t width, int32_t height)
{
    (void)output;
    qwswl_state_t *state = (qwswl_state_t *)data;
    
    WL_TRACE("xdg_output: logical_size %dx%d", width, height);

    if (state->screen_width == 0 && state->screen_height == 0) {
        state->screen_width = width;
        state->screen_height = height;
    } else {
        WL_TRACE("WARNING: screen geometry was forced via user input");
    }
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *output)
{
    (void)data; (void)output;
    WL_TRACE("xdg_output: done");
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *output,
                             const char *name)
{
    (void)data; (void)output;
    WL_TRACE("xdg_output: name \"%s\"", name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *output,
                                    const char *description)
{
    (void)data; (void)output;
    WL_TRACE("xdg_output: description \"%s\"", description);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_logical_position,
    .logical_size     = xdg_output_logical_size,
    .done             = xdg_output_done,
    .name             = xdg_output_name,
    .description      = xdg_output_description,
};

/* ================================================================
 * Initialization
 * ================================================================ */

int qwswl_init(qwswl_state_t *state, int qws_display,
                int32_t width, int32_t height, int32_t depth,
                qws_ipc_type_t ipc_type)
{
    memset(state, 0, sizeof(*state));
    state->ipc_type = ipc_type;
    state->qws_server_fd = -1;
    state->epoll_fd = -1;
    state->screen_width = width;
    state->screen_height = height;
    state->screen_depth = depth;
    state->next_window_id = 1;
    state->top_window = NULL;
    state->running = true;

    /* Initialize client slots */
    for (int i = 0; i < QWSWL_MAX_CLIENTS; i++) {
        state->clients[i].fd = -1;
        state->clients[i].in_use = false;
        qws_reader_init(&state->clients[i].reader, true);
    }

    /* Initialize window slots */
    for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
        state->windows[i].in_use = false;
        state->windows[i].allocated = false;
        state->windows[i].shm_fd = -1;
    }

    state->xkb_context = xkb_context_new(0);
    if (!state->xkb_context) {
        fprintf(stderr, "[qwswayland] Failed to create xkb context\n");
        return -1;
    }

    /* ---- Connect to Wayland ---- */
    state->wl_display = wl_display_connect(NULL);
    if (!state->wl_display) {
        fprintf(stderr, "[qwswayland] Failed to connect to Wayland display\n");
        return -1;
    }

    state->wl_registry = wl_display_get_registry(state->wl_display);
    wl_registry_add_listener(state->wl_registry, &registry_listener, state);

    /* Round-trip to get globals */
    wl_display_roundtrip(state->wl_display);

    if (!state->wl_compositor) {
        fprintf(stderr, "[qwswayland] No wl_compositor found\n");
        return -1;
    }
    if (!state->wl_shm) {
        fprintf(stderr, "[qwswayland] No wl_shm found\n");
        return -1;
    }
    
    if (!state->wl_output) {
        fprintf(stderr, "[qwswayland] No wl_output found\n");
        return -1;
    }
    
    if (!state->xdg_output_manager) {
        fprintf(stderr, "[qwswayland] No xdg_output_manager found\n");
        return -1;
    }

    if (!state->xdg_wm_base) {
        fprintf(stderr, "[qwswayland] No xdg_wm_base found\n");
        return -1;
    }

    /* Add seat listener if we got a seat. 
     *
     * For some weird reason this needs to be registered first (at least before the 
     * xdg_output_manager). Otherwise, we will be able to register the listener, but will not 
     * get a wl_seat_capability callback anymore and therefore won't have any input. */
    if (state->wl_seat) {
        wl_seat_add_listener(state->wl_seat, &seat_listener, state);
        wl_display_roundtrip(state->wl_display);
    } else {
        fprintf(stderr, 
            "[qwswayland] WARNING: No wl_seat found - input devices will be unavailable\n" \
            "             THIS IS LIKELY NOT WHAT YOU WANT - EVEN WINDOW DRAWING COULD BE AFFECTED!\n");
    }

    state->xdg_output = 
        zxdg_output_manager_v1_get_xdg_output(state->xdg_output_manager, state->wl_output);
    if (!state->xdg_output) {
        fprintf(stderr, "[qwswayland] Failed to create xdg_output for wl_output\n");
        return -1;
    }
    zxdg_output_v1_add_listener(state->xdg_output, &xdg_output_listener, state);
    wl_display_roundtrip(state->wl_display);

    xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, NULL);
    wl_display_roundtrip(state->wl_display);

    /* ---- Compute socket path once ---- */
    if (qws_socket_path(qws_display, state->socket_path,
                         sizeof(state->socket_path)) != 0) {
        fprintf(stderr, "[qwswayland] Failed to compute socket path\n");
        return -1;
    }

    /* ---- Create QWS server socket ---- */
    state->qws_server_fd = qws_server_listen(state->socket_path);
    if (state->qws_server_fd < 0) {
        fprintf(stderr, "[qwswayland] Failed to create QWS socket at %s: %s\n",
                state->socket_path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[qwswayland] QWS server listening on %s\n",
            state->socket_path);

    /* ---- Create shared memory region for display props ---- */
    if (qws_shm_create(&state->display_shm, QWSWL_SHM_SIZE, state->ipc_type) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create display shm\n");
        return -1;
    }

    /* ---- Create display lock ---- */
    if (qws_display_lock_create(&state->display_lock, state->ipc_type,
                                  state->socket_path, 'd', true) != 0) {
        fprintf(stderr, "[qwswayland] Failed to not create display lock\n");
        return -1;
    }

    /* ---- Set up epoll ---- */
    state->epoll_fd = epoll_create1(0);
    if (state->epoll_fd < 0) {
        perror("[qwswayland] epoll_create1");
        return -1;
    }

    /* Watch QWS server socket for new connections */
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.fd = state->qws_server_fd };
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, state->qws_server_fd, &ev);
    }

    /* Watch Wayland display fd for events */
    {
        int wl_fd = wl_display_get_fd(state->wl_display);
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.fd = wl_fd };
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev);
    }

    fprintf(stderr, "[qwswayland] Initialized: %dbpp\n", depth);
    return 0;
}

/* ================================================================
 * Client management
 * ================================================================ */

int qwswl_accept_client(qwswl_state_t *state)
{
    int fd = qws_server_accept(state->qws_server_fd);
    if (fd < 0)
        return -1;

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < QWSWL_MAX_CLIENTS; i++) {
        if (!state->clients[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        fprintf(stderr, "[qwswayland] Max clients reached, rejecting\n");
        close(fd);
        return -1;
    }

    qwswl_client_t *cl = &state->clients[idx];
    cl->fd = fd;
    cl->client_id = idx + 1;
    cl->in_use = true;
    cl->next_id = (idx + 1) * 1000;  /* ID space per client */
    qws_reader_init(&cl->reader, true);

    /* Add to epoll */
    struct epoll_event ev = { .events = EPOLLIN,
                               .data.fd = fd };
    epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    fprintf(stderr, "[qwswayland] Client connected (fd=%d)\n",
            fd);
    return idx;
}

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl)
{
    assert(cl->in_use);

    fprintf(stderr, "[qwswayland] Client %d disconnected\n", cl->client_id);

    /* Destroy all windows belonging to this client */
    for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
        if (state->windows[i].in_use &&
            state->windows[i].client == cl) {
            qwswl_destroy_window(state, &state->windows[i]);
        }
    }

    /* Detach from client's lock (we don't own it, client does) */
    qws_lock_destroy(&cl->lock);

    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);
    close(cl->fd);
    qws_reader_reset(&cl->reader);
    cl->fd = -1;
    cl->in_use = false;
}

/* ================================================================
 * Command dispatch
 * ================================================================ */

void qwswl_handle_client_data(qwswl_state_t *state, qwswl_client_t *cl)
{
    uint8_t buf[4096];

    ssize_t n = read(cl->fd, buf, sizeof(buf));
    if (n <= 0) {
        qwswl_disconnect_client(state, cl);
        return;
    }

    /* Raw wire trace at max verbosity */
    qws_trace_raw_bytes(cl->client_id, buf, (size_t)n);

    size_t offset = 0;
    while (offset < (size_t)n) {
        qws_packet_t *pkt = NULL;
        size_t consumed = qws_reader_feed(&cl->reader,
                                            buf + offset,
                                            (size_t)n - offset,
                                            &pkt);
        if (consumed == 0) break;
        offset += consumed;

        if (pkt) {
            qws_trace_packet(cl->client_id, pkt, false);
            qwswl_dispatch_command(state, cl, pkt);
            qws_packet_free(pkt);
        }
    }
}

void qwswl_dispatch_command(qwswl_state_t *state, qwswl_client_t *cl,
                             qws_packet_t *incoming_pkt)
{
    int32_t type = incoming_pkt->header.type;

    switch (type) {

    case QWS_CMD_IDENTIFY: {
        /* First command from client after connecting.
         * Contains the app name and the client's lock semaphore ID.
         * We need to attach to the client's lock so we can synchronize
         * Communication and RegionEvent signaling. */
        qws_cmd_identify_t *cmd = (qws_cmd_identify_t *)incoming_pkt->simple_data;
        if (!cmd) break;
 
        /* Lock ID is in simpleData.id_lock */
        int32_t lock_id = cmd->id_lock;
 
        if (lock_id >= 0) {
            if (qws_lock_open(&cl->lock, state->ipc_type, lock_id) == 0) {
                fprintf(stderr, "[qwswayland] Client %d: attached to lock id=%d (%s)\n",
                        cl->client_id, lock_id,
                        state->ipc_type == QWS_IPC_SYSV ? "SysV" : "POSIX");
            } else {
                fprintf(stderr, "[qwswayland] Client %d: FAILED to attach lock id=%d\n",
                        cl->client_id, lock_id);
            }
        } else {
            fprintf(stderr, "[qwswayland] Client %d: error no lock id in Identify\n", cl->client_id);
        }

        /* Send Connected event */
        int32_t shm_id = (state->display_shm.shm_id >= 0)
                            ? state->display_shm.shm_id : -1;
        char display_spec[128];
        snprintf(display_spec, sizeof(display_spec), "vnc:size=%dx%d:depth=%d", state->screen_width, 
            state->screen_height, state->screen_depth);
        qws_packet_t *conn = qws_make_connected_event(
            cl->client_id, shm_id, display_spec);
        qws_trace_packet(cl->client_id, conn, true);
        qws_write_packet(cl->fd, conn);

        /*
        * The client blocks in waitForCreation() after connecting and
        * will not proceed until it receives a Creation event. The server
        * must proactively provide IDs — the client does not send a
        * Create command first. Therefore, the first window is allocated
        * by default. */
        qwswl_window_t *window = qwswl_create_window(state, NULL, cl, true);
    
        qws_packet_t *cre = qws_make_creation_event(window->qws_id, 1);
        qws_trace_packet(cl->client_id, cre, true);
        qws_write_packet(cl->fd, cre);

        break;
    }

    case QWS_CMD_CREATE: {
        /* Client requests `count` new object IDs.
         * Server allocates a contiguous range and responds with a single
         * Creation event containing the first ID and the count.
         * These are generic IDs — the client will later use them for
         * windows, properties, etc. We don't create windows here. */
        qws_cmd_create_t *cmd = (qws_cmd_create_t *)incoming_pkt->simple_data;
        assert(cmd->count >= 0);
 
        int32_t first_id = state->next_window_id;
        for (int32_t i = 0; i < cmd->count; i++) {
            qwswl_window_t *wnd = &state->windows[first_id + i - 1];
            assert(!wnd->allocated);
            wnd->allocated = true;
            wnd->qws_id = state->next_window_id++;
        }
 
        qws_packet_t *evt = qws_make_creation_event(first_id, cmd->count);
        qws_trace_packet(cl->client_id, evt, true);
        qws_write_packet(cl->fd, evt);
        break;
    }

    case QWS_CMD_REGION: {
        /* Client requests a screen region for its window */
        qws_cmd_region_request_t *cmd =
            (qws_cmd_region_request_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        if (!win) {
            fprintf(stderr, "[qwswayland] Illegal window id %d from client %d\n",
                cmd->window, cl->fd);
            break;
        }

        /* Extract raw data */
        int32_t nrects = cmd->nrectangles;
        qws_rect_t *rects = (qws_rect_t *)incoming_pkt->raw_data;
        char * surface_key;
        uint8_t *surface_data = 
            &((char *) incoming_pkt->raw_data)[cmd->nrectangles * sizeof(qws_rect_t) + cmd->surfacekeylength * 2];

        if (qws_convert_to_narrow_unicode(&surface_key, (const wchar_t *)
                &((char *) incoming_pkt->raw_data)[cmd->nrectangles * sizeof(qws_rect_t)],
                cmd->surfacekeylength * 2) < 0) {
            fprintf(stderr, "[qwswayland] Illegal surface key from client %d\n",
                cl->client_id);
            break;
        }

        int32_t width, height;
        if (strcmp(surface_key, "shm") == 0 &&
                cmd->surfacedatalength >= (int32_t)sizeof(qws_cmd_region_surface_data_shm_t)) {
            const qws_cmd_region_surface_data_shm_t *sd =
                (const qws_cmd_region_surface_data_shm_t *)surface_data;
            win->client_shm_id = sd->mem_id;
            height = sd->height;
            width = sd->width;
        }
        free(surface_key);

        if (nrects > 0) {
            /* In QWS, the server allocates the region back to the client.
             * For our proxy, we grant the full requested region and also
             * use it to size the Wayland surface. */
            qwswl_update_window_region(state, win, rects, 
                nrects, width, height);
            qwswl_commit_surface(state, win, NULL, 0);
        } else {
            /* The QWS client wants us to hide the surface temporarily, so
             * will re-attach a NULL buffer as content, so that the compositor
             * won't have anything to render anymore. */
            wl_surface_attach(win->wl_surface, NULL, 0, 0);
            wl_surface_commit(win->wl_surface);
        }

        /* Send the region back as granted */
        qws_packet_t *evt = qws_make_region_event(
            cmd->window, 0 /* allocation */, rects, nrects);
        qws_trace_packet(cl->client_id, evt, true);
        qws_write_packet(cl->fd, evt);
        
        qws_lock_unlock(&cl->lock, QWS_LOCK_REGIONEVENT);

        break;
    }

    case QWS_CMD_REGION_NAME: {
        char *region_name, *region_caption;
        /* Client sets window title */
        qws_cmd_region_name_t *cmd =
            (qws_cmd_region_name_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_window(state, cmd->window, false);
        assert(win && win->allocated);
        if (!win->in_use) {
            /* Create a window on-demand if the id has already been allocated before */
            assert(qwswl_create_window(state, win, cl, false));
            qwswl_commit_surface(state, win, NULL, 0);
        }

        if (qws_convert_to_narrow_unicode(&region_name, (const wchar_t *)
                incoming_pkt->raw_data, cmd->name_len * 2) < 0) {
            fprintf(stderr, "[qwswayland] Illegal region name from client %d\n",
                cl->client_id);
            break;
        }
        if (qws_convert_to_narrow_unicode(&region_caption, (const wchar_t *)
                &((char *) incoming_pkt->raw_data)[cmd->name_len * 2], cmd->caption_len * 2) < 0) {
            fprintf(stderr, "[qwswayland] Illegal region caption from client %d\n",
                cl->client_id);
            free(region_name);
        }

        qwswl_set_window_name(state, win, region_name, region_caption);

        free(region_name);
        free(region_caption);

        break;
    }

    case QWS_CMD_REGION_MOVE: {
        qws_cmd_region_move_t *cmd =
            (qws_cmd_region_move_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        assert(win);
        win->geometry.x += cmd->dx;
        win->geometry.y += cmd->dy;

        break;
    }
    case QWS_CMD_REGION_DESTROY: {
        qws_cmd_region_destroy_t *cmd =
            (qws_cmd_region_destroy_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        assert(win);
        qwswl_destroy_window(state, win);
        break;
    }

    case QWS_CMD_CHANGE_ALTITUDE: {
        qws_cmd_change_altitude_t *cmd =
            (qws_cmd_change_altitude_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        if (!win) {
            fprintf(stderr, "[qwswayland] Illegal window id %d from client %d\n",
                cmd->window, cl->fd);
            break;
        }
        if (!win->parent) {
            break;
        }
        assert(win->wl_subsurface);

        switch (cmd->altitude) {
            case QWS_ALTITUDE_STAYS_ON_TOP:
            case QWS_ALTITUDE_RAISE:
                wl_subsurface_place_above(win->wl_subsurface, win->parent->wl_surface);
                break;
            case QWS_ALTITUDE_LOWER:
                wl_subsurface_place_below(win->wl_subsurface, win->parent->wl_surface);
                break;
        }

        qwswl_commit_surface(state, win, NULL, 0);
        break;
    }

    case QWS_CMD_REQUEST_FOCUS: {
        qws_cmd_request_focus_t *cmd =
            (qws_cmd_request_focus_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        if (win)
            qwswl_focus_window(state, win);
        break;
    }

    case QWS_CMD_SET_OPACITY: {
        qws_cmd_set_opacity_t *cmd =
            (qws_cmd_set_opacity_t *)incoming_pkt->simple_data;
        if (!cmd) break;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        if (win)
            win->opacity = cmd->opacity;
        /* Wayland doesn't have per-surface opacity natively.
         * Could use wp_alpha_modifier or compositor-specific protocol. */
        break;
    }

    case QWS_CMD_REPAINT_REGION: {
        /* Client has finished painting, requests compositor to show it */
        qws_cmd_repaint_region_t *cmd =
            (qws_cmd_repaint_region_t *)incoming_pkt->simple_data;
        qws_rect_t *rects = (qws_rect_t *)incoming_pkt->raw_data;

        qwswl_window_t *win = qwswl_find_in_use_window(state, cmd->window);
        assert(win);
        // qwswl_commit_surface(state, win, rects, cmd->nrectangles);
        qwswl_commit_surface(state, win, NULL, 0);

        qws_lock_unlock(&cl->lock, QWS_LOCK_REGIONEVENT);
        
        break;
    }

    case QWS_CMD_GRAB_MOUSE: {
        /* Acknowledge but don't actually do anything special */
        break;
    }

    case QWS_CMD_GRAB_KEYBOARD: {
        break;
    }

    case QWS_CMD_ADD_PROPERTY:
    case QWS_CMD_SET_PROPERTY:
    case QWS_CMD_REMOVE_PROPERTY:
    case QWS_CMD_GET_PROPERTY: {
        /* Minimal property store - most QWS apps don't rely heavily on this.
         * TODO: implement a simple key-value store if needed. */
        if (type == QWS_CMD_GET_PROPERTY) {
            qws_cmd_get_property_t *cmd =
                (qws_cmd_get_property_t *)incoming_pkt->simple_data;
            if (cmd) {
                /* Send empty reply */
                qws_packet_t *reply = qws_make_property_reply(
                    cmd->window, cmd->property, NULL, 0);
                qws_trace_packet(cl->client_id, reply, true);
                qws_write_packet(cl->fd, reply);
            }
        }
        break;
    }

    case QWS_CMD_DEFINE_CURSOR:
    case QWS_CMD_SELECT_CURSOR:
    case QWS_CMD_POSITION_CURSOR: {
        /* TODO: translate to Wayland cursor via wl_cursor_theme */
        break;
    }

    case QWS_CMD_QCOP_REGISTER:
    case QWS_CMD_QCOP_SEND: {
        /* QCop IPC: could be bridged to D-Bus or handled internally.
         * For now, silently ignore. */
        break;
    }

    case QWS_CMD_IM_UPDATE: {
        /* A widget is reporting an IM focus or context change.
         * FocusIn/FocusOut tell us which widget_id holds the IM cursor;
         * Update signals that the cursor position or selection changed.
         * We don't forward this to the Wayland compositor — Wayland IM
         * (text-input-v3 / input-method-v2) is managed separately.
         * No response is expected by the client for this command. */
        break;
    }

    case QWS_CMD_FONT: {
        /* Font usage notifications - not needed for our proxy */
        break;
    }

    case QWS_CMD_SHUTDOWN: {
        fprintf(stderr, "[qwswayland] Client %d requested shutdown\n",
                cl->client_id);
        qwswl_disconnect_client(state, cl);
        break;
    }

    default:
        fprintf(stderr, "[qwswayland] Unhandled command 0x%x from client %d\n",
                type, cl->client_id);
        break;
    }

    if (qws_is_synchronous_commmand(type)) {
        /* Increment communication semaphore after completing synchronous commands.
         * The client blocks in sendSynchronousCommand() and won't process the event 
         * until it can decrement the semaphore. The server has to increment the semaphore
         * first after sending the event. */
        qws_lock_unlock(&cl->lock, QWS_LOCK_COMMUNICATION);
    }
}

/* ================================================================
 * Window management
 * ================================================================ */


qwswl_window_t *qwswl_find_window(qwswl_state_t *state, int32_t qws_id, bool in_use)
{
    for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
        qwswl_window_t *window = &state->windows[i];
        if (window->allocated && window->qws_id == qws_id) {
            if (in_use && !window->in_use)
                break;
            return window;
        }
    }
    return NULL;
}

static qwswl_window_t *qwswl_find_in_use_window(qwswl_state_t *state, int32_t qws_id)
{
    return qwswl_find_window(state, qws_id, true);
}

/* -----------------------------------------------------------
 * xdg-shell listeners
 * ----------------------------------------------------------- */

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                   uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height,
                                    struct wl_array *states)
{
    (void)toplevel; (void)states;
    qwswl_window_t *win = (qwswl_window_t *)data;
    /* width/height of 0 means "compositor defers to the client" */
    if (width > 0 && height > 0) {
        assert(false);
        // win->geometry.width  = width;
        // win->geometry.height = height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    qwswl_window_t *win = (qwswl_window_t *)data;
    win->visible = false;
}

static void xdg_toplevel_configure_bounds(void *data, 
    struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height) 
{
    qwswl_window_t *win = (qwswl_window_t *)data;
    qwswl_client_t *cl = win->client;

    /* Send MaxWindowRect */
    qws_packet_t *mwr = qws_make_max_window_rect_event(
        win->qws_id, 0, 0, width, height);
    qws_trace_packet(cl->client_id, mwr, true);
    qws_write_packet(cl->fd, mwr);
}

static void xdg_wm_capabilities(void *data, 
    struct xdg_toplevel *xdg_toplevel, struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_wm_capabilities,
};

qwswl_window_t *qwswl_create_window(qwswl_state_t *state, qwswl_window_t *win, 
    qwswl_client_t *client, bool is_first)
{
    /* Find free slot */
    if (!win && !is_first) {
        for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
            if (!state->windows[i].in_use && state->windows[i].allocated) {
                win = &state->windows[i];
                break;
            }
        }
        if (!win) return NULL;
    } else if (is_first) {
        assert(!win);
        win = &state->windows[0];
        win->allocated = true;
        win->qws_id = state->next_window_id++;
    }

    assert(!win->in_use && (win->allocated || is_first));

    win->client = client;
    win->shm_fd = -1;
    win->opacity = 255;
    win->in_use = true;

    /* Create Wayland surface */
    win->wl_surface = wl_compositor_create_surface(state->wl_compositor);

    if (is_first) {
        /* Toplevel window: wrap in xdg_surface + xdg_toplevel.
         * Listeners must be added before the initial commit so we don't
         * miss the configure event that the compositor sends in response. */
        win->xdg_surface  = xdg_wm_base_get_xdg_surface(state->xdg_wm_base,
                                                          win->wl_surface);
        win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
        xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
        xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);
    } else {
        /* Child window: attach as a subsurface of the root toplevel.
         * No xdg-shell role or configure handshake needed. */
        win->parent = &state->windows[0];
        win->wl_subsurface = wl_subcompositor_get_subsurface(
            state->wl_subcompositor, win->wl_surface, win->parent->wl_surface);
        // wl_subsurface_set_sync(win->wl_subsurface);
    }

    wl_surface_set_user_data(win->wl_surface, (void *) win);
    wl_surface_commit(win->wl_surface);

    fprintf(stderr, "[qwswayland] Created window %d for client %d\n",
            win->qws_id, client->client_id);
    return win;
}

void qwswl_destroy_window(qwswl_state_t *state, qwswl_window_t *win)
{
    if (!win || !win->in_use) return;

    fprintf(stderr, "[qwswayland] Destroying window %d\n", win->qws_id);

    /* Destroy Wayland objects */
    if (win->wl_buffer)
        wl_buffer_destroy(win->wl_buffer);
    if (win->xdg_toplevel) {
        xdg_toplevel_destroy(win->xdg_toplevel);
    }
    if (win->xdg_surface) {
        xdg_surface_destroy(win->xdg_surface);
    }
    if (win->wl_surface)
        wl_surface_destroy(win->wl_surface);

    /* Clean up shm */
    if (win->shm_pixels && win->shm_size > 0) {
        munmap(win->shm_pixels, win->shm_size);
    }
    if (win->shm_fd >= 0)
        close(win->shm_fd);

    memset(win, 0, sizeof(*win));
    win->shm_fd = -1;
    win->in_use = false;
}

void qwswl_update_window_region(qwswl_state_t *state, qwswl_window_t *win,
                                const qws_rect_t *rects, int32_t nrects,
                                int32_t width, int32_t height)
{
    if (!win || nrects <= 0 || !rects) return;

    /* Compute bounding box of the region */
    int32_t min_x1 = rects[0].x1, min_y1 = rects[0].y1;
    int32_t max_x2 = rects[0].x2, max_y2 = rects[0].y2;

    for (int32_t i = 1; i < nrects; i++) {
        if (rects[i].x1 < min_x1) min_x1 = rects[i].x1;
        if (rects[i].y1 < min_y1) min_y1 = rects[i].y1;
        if (rects[i].x2 > max_x2) max_x2 = rects[i].x2;
        if (rects[i].y2 > max_y2) max_y2 = rects[i].y2;
    }

    win->geometry.x = min_x1;
    win->geometry.y = min_y1;
    win->geometry.width = width;
    win->geometry.height = height;

    if (win->parent) {
        qwswl_window_t *parent = win->parent;
        assert(win->wl_subsurface);

        /* for lower-level windows the position of the subsurface is relative to
         * the parent, so we need to compensate the offset between the two windows */
        wl_subsurface_set_position(win->wl_subsurface, 
            win->geometry.x - parent->geometry.x, 
            win->geometry.y - parent->geometry.y);
    }

    /* (Re)create the Wayland buffer to match new size */
    qwswl_create_buffer(state, win, win->geometry.width, win->geometry.height);
}

void qwswl_set_window_name(qwswl_state_t *state, qwswl_window_t *win,
                             const char *name, const char *caption)
{
    (void)state; (void)caption;
    if (!name || !win || !win->xdg_toplevel) return;

    xdg_toplevel_set_title(win->xdg_toplevel, caption);
}

void qwswl_focus_window(qwswl_state_t *state, qwswl_window_t *win)
{
    (void)state; (void)win;
    /* Similarly, Wayland focus is compositor-driven.
     * For embedded use with our own compositor, we'd handle this
     * via a private protocol. */
}

/* ================================================================
 * Pixel buffer management
 * ================================================================ */

int qwswl_create_buffer(qwswl_state_t *state, qwswl_window_t *win,
                          int32_t width, int32_t height)
{
    if (!state->wl_shm || !win->wl_surface)
        return -1;

    /* Destroy old buffer if any */
    if (win->wl_buffer) {
        wl_buffer_destroy(win->wl_buffer);
        win->wl_buffer = NULL;
    }
    if (win->shm_pixels && win->shm_size > 0) {
        munmap(win->shm_pixels, win->shm_size);
        win->shm_pixels = NULL;
    }
    if (win->shm_fd >= 0) {
        close(win->shm_fd);
        win->shm_fd = -1;
    }

    int32_t stride = width * 4;  /* ARGB32 = 4 bytes/pixel */
    size_t size = (size_t)(stride * height);

    /* Create anonymous file for wl_shm */
    char template[] = "/tmp/qwswl-shm-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    unlink(template);

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }

    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return -1;
    }

    /* Create wl_shm_pool and wl_buffer */
    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, (int32_t)size);
    if (!pool) {
        munmap(pixels, size);
        close(fd);
        return -1;
    }

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    if (!buffer) {
        munmap(pixels, size);
        close(fd);
        return -1;
    }

    win->wl_buffer = buffer;
    win->shm_pixels = pixels;
    win->shm_fd = fd;
    win->shm_size = size;

    return 0;
}

void qwswl_commit_surface(qwswl_state_t *state, qwswl_window_t *win,
                          const qws_rect_t *rects, int32_t nrects)
{
    const qws_rect_t default_rect = 
        {0, 0, win->geometry.width, win->geometry.height };
    void *src;

    if (!win || !win->wl_surface || !win->wl_buffer)
        return;

    if (!rects) {
        assert(nrects == 0);
        nrects = 1;
        rects = &default_rect;
    }

    src = shmat(win->client_shm_id, NULL, SHM_RDONLY);
    if (src == (void *)-1) {
        WL_TRACE("failed to allocate shared memory - window already ready?");
        return;
    }

    wl_surface_attach(win->wl_surface, win->wl_buffer, 0, 0);

    for (int i = 0; i < nrects; i++) {
        uint32_t copy_x = rects[i].x1;
        uint32_t copy_y = rects[i].y1;
        uint32_t copy_w = rects[i].x2 - rects[i].x1;
        uint32_t copy_h = rects[i].y2 - rects[i].y1;
        
        // if (win->parent) {
        //     qwswl_geometry_t *p_geometry = &win->parent->geometry;
        //     copy_x -= p_geometry->x;
        //     copy_y -= p_geometry->y;
        // }

        uint32_t row_offset = copy_x * 4;
        uint32_t row_bytes = win->geometry.width * 4;

        /* TODO: format conversion (e.g. RGB16 → ARGB32) when client_format != ARGB32 */
        for (uint32_t y = 0; y < copy_h; y++) {
            uint32_t off = row_offset + ((copy_y + y) * row_bytes);
            memcpy((uint8_t *) win->shm_pixels + off,
                   (const uint8_t *) src + off,
                   copy_w * 4);
        }

        wl_surface_damage(win->wl_surface, copy_x, copy_y, copy_w, copy_h);
    }
    shmdt(src);

    wl_surface_commit(win->wl_surface);
    // if (win->parent)
        // wl_surface_commit(win->parent->wl_surface);

    wl_display_flush(state->wl_display);
}

/* ================================================================
 * Main event loop
 * ================================================================ */

int qwswl_run(qwswl_state_t *state)
{
    struct epoll_event events[32];
    int wl_fd = wl_display_get_fd(state->wl_display);

    fprintf(stderr, "[qwswayland] Entering main loop\n");

    while (state->running) {
        /* Flush Wayland before blocking */
        while (wl_display_prepare_read(state->wl_display) != 0)
            wl_display_dispatch_pending(state->wl_display);
        wl_display_flush(state->wl_display);

        int nfds = epoll_wait(state->epoll_fd, events, 32, 100);

        if (nfds < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(state->wl_display);
                continue;
            }
            wl_display_cancel_read(state->wl_display);
            perror("[qwswayland] epoll_wait");
            break;
        }

        bool had_wayland = false;

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == state->qws_server_fd) {
                /* New QWS client connection */
                qwswl_accept_client(state);
            } else if (fd == wl_fd) {
                /* Wayland events */
                had_wayland = true;
            } else {
                /* Data from a QWS client */
                for (int i = 0; i < QWSWL_MAX_CLIENTS; i++) {
                    qwswl_client_t *cl = &state->clients[i];
                    if (cl->in_use && cl->fd == fd) {
                        qwswl_handle_client_data(state, cl);
                        break;
                    }
                }
            }
        }

        if (had_wayland) {
            wl_display_read_events(state->wl_display);
        } else {
            wl_display_cancel_read(state->wl_display);
        }
        wl_display_dispatch_pending(state->wl_display);
    }

    return 0;
}

/* ================================================================
 * Shutdown
 * ================================================================ */

void qwswl_shutdown(qwswl_state_t *state)
{
    fprintf(stderr, "[qwswayland] Shutting down\n");

    /* Disconnect all clients */
    for (int i = 0; i < QWSWL_MAX_CLIENTS; i++) {
        qwswl_client_t *cl = &state->clients[i];
        if (cl->in_use)
            qwswl_disconnect_client(state, cl);
    }

    /* Destroy remaining windows */
    for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
        if (state->windows[i].in_use)
            qwswl_destroy_window(state, &state->windows[i]);
    }

    /* Clean up Wayland */
    if (state->wl_pointer)
        wl_pointer_destroy(state->wl_pointer);
    if (state->wl_keyboard) {
        qwswl_keyboard_data_t *kbd_data =
            wl_keyboard_get_user_data(state->wl_keyboard);
        if (kbd_data) {
            xkb_state_unref(kbd_data->xkb_state);
            xkb_keymap_unref(kbd_data->xkb_keymap);
            free(kbd_data);
        }
        wl_keyboard_destroy(state->wl_keyboard);
    }
    if (state->xkb_context)
        xkb_context_unref(state->xkb_context);
    if (state->wl_seat)
        wl_seat_destroy(state->wl_seat);
    if (state->xdg_wm_base)
        xdg_wm_base_destroy(state->xdg_wm_base);
    if (state->xdg_output_manager)
        zxdg_output_manager_v1_destroy(state->xdg_output_manager);
    if (state->wl_subcompositor)
        wl_subcompositor_destroy(state->wl_subcompositor);
    if (state->wl_shm)
        wl_shm_destroy(state->wl_shm);
    if (state->wl_compositor)
        wl_compositor_destroy(state->wl_compositor);
    if (state->wl_output)
        wl_output_destroy(state->wl_output);
    if (state->wl_registry)
        wl_registry_destroy(state->wl_registry);
    if (state->wl_display) {
        wl_display_flush(state->wl_display);
        wl_display_disconnect(state->wl_display);
    }

    /* Clean up QWS */
    if (state->qws_server_fd >= 0) {
        close(state->qws_server_fd);
        unlink(state->socket_path);
    }

    qws_shm_destroy(&state->display_shm);
    qws_display_lock_destroy(&state->display_lock);

    if (state->epoll_fd >= 0)
        close(state->epoll_fd);
}