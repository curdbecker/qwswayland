
/*
 * lifecycle.c - QWSWayland proxy daemon startup and shutdown handling
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "lifecycle.h"
#include "client.h"
#include "cursor.h"
#include "debug.h"
#include "proxy.h"
#include "qws_trace.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <assert.h>

#define T qwswl_client_map_t, int32_t, qwswl_client_t *
#define i_declared
#include "stc/hashmap.h"

#include "linuxfb_interposer.h"

extern const struct wl_seat_listener seat_listener;

/* ================================================================
 * xdg_wm_base listener (keepalive ping/pong)
 * ================================================================ */

static void xdg_base_wm_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_base_wm_ping,
};

/* ================================================================
 * wl_output listener (fallback when xdg_output_manager is absent)
 * ================================================================ */

static void wl_output_geometry(void *data, struct wl_output *output, int32_t x,
                               int32_t y, int32_t physical_width,
                               int32_t physical_height, int32_t subpixel,
                               const char *make, const char *model,
                               int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)transform;
    QWS_TRACE("wl_output: geometry make=\"%s\" model=\"%s\"", make, model);
}

static void wl_output_mode(void *data, struct wl_output *output, uint32_t flags,
                           int32_t width, int32_t height, int32_t refresh) {
    (void)output;
    (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    qwswl_state_t *state = (qwswl_state_t *)data;
    QWS_TRACE("wl_output: mode %dx%d", width, height);
    if (state->screen_width == 0 && state->screen_height == 0) {
        state->screen_width = width;
        state->screen_height = height;
        QWS_TRACE("wl_output: corrected_size %dx%d", state->screen_width,
                  state->screen_height);
    } else {
        QWS_TRACE("WARNING: screen geometry was forced via user input");
    }
}

static void wl_output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
    QWS_TRACE("wl_output: done");
}

static void wl_output_scale(void *data, struct wl_output *output,
                            int32_t factor) {
    (void)data;
    (void)output;
    QWS_TRACE("wl_output: scale %d", factor);
}

static void wl_output_name(void *data, struct wl_output *output,
                           const char *name) {
    (void)data;
    (void)output;
    QWS_TRACE("wl_output: name \"%s\"", name);
}

static void wl_output_description(void *data, struct wl_output *output,
                                  const char *description) {
    (void)data;
    (void)output;
    QWS_TRACE("wl_output: description \"%s\"", description);
}

static const struct wl_output_listener wl_output_listener = {
    .geometry = wl_output_geometry,
    .mode = wl_output_mode,
    .done = wl_output_done,
    .scale = wl_output_scale,
    .name = wl_output_name,
    .description = wl_output_description,
};

/* ================================================================
 * Wayland registry listener
 * ================================================================ */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    qwswl_state_t *state = (qwswl_state_t *)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        state->wl_compositor =
            wl_registry_bind(reg, name, &wl_compositor_interface, version);
        QWS_TRACE("registry: found wl_compositor (name=%u, max_version=%u)",
                  name, version);
    } else if (strcmp(interface, "wl_shm") == 0) {
        int selected_version = 1;
        state->wl_shm =
            wl_registry_bind(reg, name, &wl_shm_interface, selected_version);
        QWS_TRACE(
            "registry: found wl_shm (name=%u, max_version=%u, selected=%d)",
            name, version, selected_version);
    } else if (strcmp(interface, "wl_seat") == 0) {
        state->wl_seat =
            wl_registry_bind(reg, name, &wl_seat_interface, version);
        state->wl_seat_name = name;
        wl_seat_add_listener(state->wl_seat, &seat_listener, state);
        QWS_TRACE("registry: found wl_seat (name=%u, max_version=%u)", name,
                  version);
        /* try to setup the clipboard - we need wl_seat and
         * wl_data_device_manager to be present and I am not too sure if the
         * registry event ordering is dependency-aware */
        qwswl_clipboard_init(state);
    } else if (strcmp(interface, "wl_data_device_manager") == 0) {
        state->clipboard.manager = wl_registry_bind(
            reg, name, &wl_data_device_manager_interface, version);
        QWS_TRACE(
            "registry: found wl_data_device_manager (name=%u, max_version=%u)",
            name, version);
        /* try to setup the clipboard - we need wl_seat and
         * wl_data_device_manager to be present and I am not too sure if the
         * registry event ordering is dependency-aware */
        qwswl_clipboard_init(state);
    } else if (strcmp(interface, "wl_output") == 0) {
        state->wl_output =
            wl_registry_bind(reg, name, &wl_output_interface, version);
        QWS_TRACE("registry: found wl_output (name=%u, max_version=%u)", name,
                  version);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        state->xdg_wm_base =
            wl_registry_bind(reg, name, &xdg_wm_base_interface, version);
        QWS_TRACE("registry: found xdg_wm_base (name=%u, max_version=%u)", name,
                  version);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        state->wl_subcompositor =
            wl_registry_bind(reg, name, &wl_subcompositor_interface, version);
        QWS_TRACE("registry: found wl_subcompositor (name=%u, max_version=%u)",
                  name, version);
    } else if (strcmp(interface, "wp_alpha_modifier_v1") == 0) {
        state->wp_alpha_modifier = wl_registry_bind(
            reg, name, &wp_alpha_modifier_v1_interface, version);
        QWS_TRACE(
            "registry: found wp_alpha_modifier_v1 (name=%u, max_version=%u)",
            name, version);
    } else if (strcmp(interface, "zqt_shell_v1") == 0) {
        state->zqt_shell =
            wl_registry_bind(reg, name, &zqt_shell_v1_interface, 1);
        QWS_TRACE("registry: found zqt_shell_v1 (name=%u, max_version=%u)",
                  name, version);
    } else if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0) {
        state->cursor.shape_manager = wl_registry_bind(
            reg, name, &wp_cursor_shape_manager_v1_interface, 1);
        QWS_TRACE("registry: found wp_cursor_shape_manager_v1 (name=%u, "
                  "max_version=%u)",
                  name, version);
    } else {
        QWS_TRACE("registry: skipped %s (name=%u, v=%u)", interface, name,
                  version);
    }
}

static void seat_teardown(qwswl_state_t *state) {
    qwswl_clipboard_destroy(state);
    if (state->wl_pointer) {
        wl_pointer_destroy(state->wl_pointer);
        state->wl_pointer = NULL;
    }
    if (state->kbd_state.xkb_state) {
        xkb_state_unref(state->kbd_state.xkb_state);
        state->kbd_state.xkb_state = NULL;
    }
    if (state->kbd_state.xkb_keymap) {
        xkb_keymap_unref(state->kbd_state.xkb_keymap);
        state->kbd_state.xkb_keymap = NULL;
    }
    if (state->wl_keyboard) {
        wl_keyboard_destroy(state->wl_keyboard);
        state->wl_keyboard = NULL;
    }
    if (state->wl_seat) {
        wl_seat_destroy(state->wl_seat);
        state->wl_seat = NULL;
    }
    state->wl_seat_name = 0;
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
    (void)reg;
    qwswl_state_t *state = (qwswl_state_t *)data;

    if (name == state->wl_seat_name && state->wl_seat) {
        QWS_TRACE("registry: wl_seat removed (name=%u)", name);
        seat_teardown(state);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ================================================================
 * Initialization
 * ================================================================ */

int qwswl_init(qwswl_state_t *state, int qws_display, bool debug_draw_rects,
               const qwswl_screen_driver_opts_t *screen_driver) {
    memset(state, 0, sizeof(*state));
    qwsprop_init(&state->prop_store);
    state->clipboard.memfd = -1;
    state->qws_display = qws_display;
    state->debug_draw_rects = debug_draw_rects;
    state->qws_server_fd = -1;
    state->qws_epoll_fd = -1;
    state->screen_width = screen_driver->width;
    state->screen_height = screen_driver->height;
    state->screen_depth = screen_driver->depth;
    state->running = true;

    qwswl_client_map_t_init();

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

    if (!state->xdg_wm_base && !state->zqt_shell) {
        fprintf(stderr,
                "[qwswayland] No supported window manager extension found\n");
        return -1;
    }

    wl_output_add_listener(state->wl_output, &wl_output_listener, state);

    if (!state->wl_seat)
        fprintf(stderr, "init: no wl_seat at startup: input devices currently "
                        "unavailable - will try to bind on registry event\n");

    if (state->xdg_wm_base)
        xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener,
                                 NULL);

    /*
     * Apparently, the safer approach to register all listeners first and
     * only then invoke a display roundtrip, since the roundtrip apparently
     * causes the compositor to emit specific events that are only emitted
     * once.
     *
     * Hence, I assume that registering another listener later on that
     * will also depend on the same events (as wl_output, xdg_output and
     * wl_seat), then we might never get a callback for one of these
     * listener again... depending on the order in which they were
     * registered. Great.
     */
    wl_display_roundtrip(state->wl_display);

    switch (screen_driver->type) {
    case QWSWL_SCREEN_DRIVER_VNC:
        snprintf((char *)&state->display_spec, sizeof(state->display_spec),
                 "vnc:size=%dx%d:depth=%d:%d", state->screen_width,
                 state->screen_height, state->screen_depth, state->qws_display);
        break;
    case QWSWL_SCREEN_DRIVER_LINUXFB: {
        qwswl_linuxfb_opts_t *opts =
            (qwswl_linuxfb_opts_t *)&screen_driver->opts;
        if (screen_driver->use_interposer) {
            uint32_t line_length = (uint32_t)state->screen_width *
                                   ((uint32_t)state->screen_depth / 8);
            linuxfb_screen_info_t fb_info = {
                .finfo =
                    {
                        .type = FB_TYPE_PACKED_PIXELS,
                        .visual = FB_VISUAL_TRUECOLOR,
                        .line_length = line_length,
                        .smem_len =
                            line_length * (uint32_t)state->screen_height,
                    },
                .vinfo =
                    {
                        .xres = (uint32_t)state->screen_width,
                        .yres = (uint32_t)state->screen_height,
                        .xres_virtual = (uint32_t)state->screen_width,
                        .yres_virtual = (uint32_t)state->screen_height,
                        .bits_per_pixel = (uint32_t)state->screen_depth,
                        /* RGB layout — Qt sums red+green+blue lengths to get
                         * depth. 32 bpp: ARGB8888  16 bpp: RGB565 */
                        .red = (state->screen_depth == 16)
                                   ? (struct fb_bitfield){11, 5, 0}
                                   : (struct fb_bitfield){16, 8, 0},
                        .green = (state->screen_depth == 16)
                                     ? (struct fb_bitfield){5, 6, 0}
                                     : (struct fb_bitfield){8, 8, 0},
                        .blue = (state->screen_depth == 16)
                                    ? (struct fb_bitfield){0, 5, 0}
                                    : (struct fb_bitfield){0, 8, 0},
                    },
            };
            char *dev = basename(opts->fb_device);
            int fd = linuxfb_shm_open(dev, true);

            if (fd < 0) {
                fprintf(
                    stderr,
                    "[qwswayland] Failed to open linuxfb shm fd for interposer\n");
                return -1;
            }
            assert(write(fd, &fb_info, sizeof(fb_info)) == sizeof(fb_info));
            close(fd);

            char fb_path[PATH_MAX];
            snprintf((char *)&state->display_spec, sizeof(state->display_spec),
                     "linuxfb:%s:%d",
                     linuxfb_interposer_path(dev, fb_path, sizeof(fb_path)),
                     state->qws_display);
        } else {
            snprintf((char *)&state->display_spec, sizeof(state->display_spec),
                     "linuxfb:%s:%d", opts->fb_device, state->qws_display);
        }

        break;
    }
    default:
        assert(false);
    }

    /* ---- Cursor infrastructure (surface + shm pool + shape device) ---- */
    if (!qwswl_cursor_init(state)) {
        fprintf(stderr, "[qwswayland] Failed to initialize cursor\n");
        return -1;
    }

    /* ---- Initialise display directory and derive all paths ---- */
    if (qws_init_display_dir(qws_display, &state->display_paths) != 0) {
        fprintf(stderr,
                "[qwswayland] Failed to initialise display directory\n");
        return -1;
    }

    if (qws_build_font_database(state->display_paths.fontdb) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create font database\n");
        return -1;
    }

    /* ---- Create QWS server socket ---- */
    state->qws_server_fd = qws_server_listen(state->display_paths.socket);
    if (state->qws_server_fd < 0) {
        fprintf(stderr, "[qwswayland] Failed to create QWS socket at %s: %s\n",
                state->display_paths.socket, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[qwswayland] QWS server listening on %s\n",
            state->display_paths.socket);

    /* Create a shared memory region from the server-side to share display
     * information with the client... and don't forget about the lock... and the
     * cursor positions hiding itself in the shared memory region.
     *
     * This looks like a legacy way in earlier Qt Versions for sharing
     * information between client and server. By now, the only real use that is
     * left is apparently the sharing of override cursors.
     *
     * Unfortunately, however, there is another subtle way in which the shared
     * memory region is still used in a quite frustratingly redundant way:
     *
     * Communication of the pointer position in the last two int32_t of the
     * shared memory region - Yes, in addition to the one sent via sockets!!!
     *
     * It seems to be rarely used, but therefore it is just even more surprising
     * when it suddenly is used and produces quite weird overall effects. For
     * instance, it is used during a move operation where it is used to
     * reinitialize the pointer position sent via Unix sockets before... hiding
     * itself in the QWS specific implementation of QCursor - see
     * `src/gui/kernel/qcursor_qws.cpp:122`.
     *
     * Therefore, the client refuses to start without it, so we unfortunately
     * need the shm region and its lock.*/
    if (qws_shm_create(&state->display_shm, QWS_DISPLAY_SHM_SIZE) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create display shm\n");
        return -1;
    }

    {
        int32_t *shm = (int32_t *)state->display_shm.base;
        int size_in_ints = state->display_shm.size / sizeof(int32_t);

        /* This will go quite wrong if shm is not int32_t aligned for
         * whatever reason... */
        assert(state->display_shm.size % sizeof(int32_t) == 0);

        /*
         * The shared cursor position is accessible via the global variables
         * `qt_last_x` and `qt_last_y` - see for instance their
         * initialisation in `src/gui/kernel/qapplication_qws.cpp:921`.
         *
         * We (hopefully) calculate the same pointers here.
         */
        state->qt_last_x = &shm[size_in_ints - 1];
        state->qt_last_y = &shm[size_in_ints - 2];
    }

    state->display_lock = qlock_create(state->display_paths.socket, 'd');
    if (state->display_lock == NULL) {
        fprintf(stderr, "[qwswayland] Failed to create display lock\n");
        return -1;
    }

    /* ---- Set up epoll ---- */
    state->qws_epoll_fd = epoll_create1(0);
    if (state->qws_epoll_fd < 0) {
        perror("[qwswayland] epoll_create1 qws_epoll_fd");
        return -1;
    }

    state->loop_epoll_fd = epoll_create1(0);
    if (state->loop_epoll_fd < 0) {
        perror("[qwswayland] epoll_create1 loop_epoll_fd");
        return -1;
    }

    state->kbd_state.repeat_timerfd =
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state->kbd_state.repeat_timerfd < 0) {
        perror("[qwswayland] timerfd_create repeat");
        return -1;
    }

    fprintf(stderr, "[qwswayland] Initialized\n");
    qwswl_debug_init(state);
    return 0;
}

/* ================================================================
 * Shutdown
 * ================================================================ */

/* disconnects the client without updating the hashmap, so it is safe
 * to iterate through it while disconnecting clients */
static void qwswl_disconnect_client_no_hashmap(qwswl_state_t *state,
                                               qwswl_client_t *cl);

void qwswl_shutdown(qwswl_state_t *state) {
    assert(state->running);

    qwswl_clipboard_destroy(state);
    qwsprop_destroy(&state->prop_store);

    fprintf(stderr, "[qwswayland] Shutting down\n");

    /* Cancel any read operation to prevent a deadlock if we should
     * have been kicked out of the epoll loop. */
    if (state->wl_display)
        wl_display_cancel_read(state->wl_display);

    /* Disconnect all clients */
    for (c_each_kv(client_id, cl, qwswl_client_map_t, state->client_map)) {
        qwswl_disconnect_client_no_hashmap(state, *cl);
    }

    /* Clean up QWS */
    if (state->qws_server_fd >= 0) {
        close(state->qws_server_fd);
        unlink(state->display_paths.socket);
    }

    qws_shm_destroy(&state->display_shm);

    if (state->display_lock != NULL)
        qlock_destroy(state->display_lock);

    if (state->qws_epoll_fd >= 0)
        close(state->qws_epoll_fd);

    if (state->loop_epoll_fd >= 0)
        close(state->loop_epoll_fd);

    /* Clean up Wayland */
    seat_teardown(state);
    qwswl_cursor_shutdown(state);
    if (state->xkb_context)
        xkb_context_unref(state->xkb_context);
    if (state->xdg_wm_base)
        xdg_wm_base_destroy(state->xdg_wm_base);
    if (state->zqt_shell)
        zqt_shell_v1_destroy(state->zqt_shell);
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

    state->running = false;
}

/* ================================================================
 * Main event loop and helpers
 * ================================================================ */

static qwswl_client_t *qwswl_accept_client(qwswl_state_t *state) {
    static int32_t next_client_id = 1;
    int fd = qws_server_accept(state->qws_server_fd);
    if (fd < 0)
        return NULL;

    qwswl_client_t *cl = qwswl_create_client(fd, next_client_id);
    assert(cl);

    /* Add to epoll */
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = (void *)cl};
    epoll_ctl(state->qws_epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    qwswl_client_map_t_result res =
        qwswl_client_map_t_insert(&state->client_map, next_client_id, cl);
    assert(res.inserted);

    next_client_id++;

    fprintf(stderr, "[qwswayland] Client connected (fd=%d)\n", fd);
    return cl;
}

static void qwswl_disconnect_client_no_hashmap(qwswl_state_t *state,
                                               qwswl_client_t *cl) {
    epoll_ctl(state->qws_epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);

    qwswl_destroy_client(state, cl);
}

void qwswl_client_foreach(qwswl_state_t *state, qwswl_client_cb_t cb,
                          void *userdata) {
    for (c_each_kv(client_id, cl, qwswl_client_map_t, state->client_map)) {
        (void)client_id;
        cb(*cl, userdata);
    }
}

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl) {
    fprintf(stderr, "[qwswayland] Client %d disconnected\n", cl->client_id);

    assert(qwswl_client_map_t_erase(&state->client_map, cl->client_id));

    qwswl_disconnect_client_no_hashmap(state, cl);
}

int qwswl_run(qwswl_state_t *state) {
    struct epoll_event loop_events[4];
    struct epoll_event qws_events[32];
    int wl_fd;

    wl_fd = wl_display_get_fd(state->wl_display);
    if (wl_fd < 0) {
        perror("[qwswayland] wl_display_get_fd");
        return 1;
    }

    /* Watch QWS server socket for new connections */
    {
        struct epoll_event ev = {.events = EPOLLIN,
                                 .data.fd = state->qws_server_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, state->qws_server_fd,
                  &ev);
    }

    /* Watch Wayland display fd for events */
    {
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = wl_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev);
    }

    /* Watch qws client epoll fd for events */ {
        struct epoll_event ev = {.events = EPOLLIN,
                                 .data.fd = state->qws_epoll_fd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD, state->qws_epoll_fd,
                  &ev);
    }

    /* Watch key-repeat timerfd */
    {
        struct epoll_event ev = {.events = EPOLLIN,
                                 .data.fd = state->kbd_state.repeat_timerfd};
        epoll_ctl(state->loop_epoll_fd, EPOLL_CTL_ADD,
                  state->kbd_state.repeat_timerfd, &ev);
    }

    fprintf(stderr, "[qwswayland] Entering main loop\n");

    /* We're using two separate epoll fds in order to comply with wayland's
     * suggested event handling procedure while still being able
     * to use wl_display_roundtrip in our own event handling code to simplify
     * pointer/key events without having to resort to stray pointers (maybe?).
     *
     * The main issue is that wl_display_roundtrip would block if the thread
     * is preparing to read, since the queue is then actually locked for
     * reading, so we would be essentially creating a deadlock. However, this
     * still would be very convenient e.g. when handling window/surface
     * destruction.
     *
     * Therefore, we're handling first reading from the wayland event queue
     * to fetch all pending events or we are cancelling the read if there are
     * no events to process. Afterwards, we're separately processing all events
     * that happened on the qws client sockets.
     *
     * The neat thing here is that an epoll fd is actually epoll-able itself,
     * so we do not have to poll separately on the chance there are QWS events,
     * but only if we are know that there must be events from our first epoll.
     * */

    while (true) {
        /* Flush Wayland before blocking */
        while (wl_display_prepare_read(state->wl_display) != 0)
            wl_display_dispatch_pending(state->wl_display);
        wl_display_flush(state->wl_display);

        bool had_wayland = false;
        bool had_qws = false;

        {
            int nfds = epoll_wait(state->loop_epoll_fd, loop_events, 3, 100);
            if (nfds < 0 && errno != EINTR) {
                if (errno == EBADFD)
                    /* we're being shut down */
                    return 0;

                perror("[qwswayland] epoll_wait loop_epoll_fd");
                return 1;
            }

            for (int i = 0; i < nfds; i++) {
                if (loop_events[i].data.fd == state->qws_server_fd) {
                    /* New QWS client connection */
                    assert(qwswl_accept_client(state));
                } else if (loop_events[i].data.fd == wl_fd) {
                    /* Wayland events */
                    had_wayland = true;
                } else if (loop_events[i].data.fd == state->qws_epoll_fd) {
                    /* QWS events */
                    had_qws = true;
                } else if (loop_events[i].data.fd ==
                           state->kbd_state.repeat_timerfd) {
                    /* Key repeat tick */
                    qwswl_keyboard_repeat_tick(state);
                }
            }
        }

        if (had_wayland) {
            wl_display_read_events(state->wl_display);
        } else {
            wl_display_cancel_read(state->wl_display);
        }

        if (had_qws) {
            int nfds = epoll_wait(state->qws_epoll_fd, qws_events, 32, 100);
            if (nfds < 0) {
                perror("[qwswayland] epoll_wait ");
                return 1;
            }

            for (int i = 0; i < nfds; i++) {
                qwswl_handle_client_data(
                    state, (qwswl_client_t *)qws_events[i].data.ptr);
            }
        }

        wl_display_flush(state->wl_display);
    }

    return 0;
}