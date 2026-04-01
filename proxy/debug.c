/*
 * debug.c - Zero-overhead window state inspection for the QWSWayland proxy
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "debug.h"
#include "client.h"
#include "window.h"
#include "qws_proto.h"

#include <stdio.h>
#include <signal.h>

/* Re-use the already-instantiated STC container code from lifecycle.c / client.c */
#define T qwswl_client_map_t, int32_t, qwswl_client_t*
#define i_declared
#include "stc/hashmap.h"

#define T qwswl_window_map_t, int32_t, qwswl_window_t*
#define i_declared
#include "stc/hashmap.h"

#define T qwswl_window_stack_t, qwswl_window_t*
#define i_declared
#include "stc/list.h"

/* ================================================================
 * Global state pointer
 * ================================================================ */

static qwswl_state_t *g_debug_state;

static void sigusr1_handler(int sig)
{
    (void)sig;
    qd_all();
}

void qwswl_debug_init(qwswl_state_t *state)
{
    g_debug_state = state;
    struct sigaction sa = { .sa_handler = sigusr1_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
}

/* ================================================================
 * Window dump
 * ================================================================ */

static void dump_window(const qwswl_window_t *win, int index)
{
    if (win->win_flags == -1)
        return;

    const qwswl_geometry_t *g = &win->geometry;

    fprintf(stderr,
            "  [%d] qws_id=%-5d  %s\n"
            "       name=\"%s\"  caption=\"%s\"\n"
            "       flags=0x%08x (%s)\n"
            "       geometry:      x=%-5d  y=%-5d  w=%-5d  h=%d\n"
            "       client_shm: fmt=%s(%d)\n"
            "       server_shm: fd=%-3d  sz=%zu\n"
            "       wl_surface=%s  wl_subsurface=%s  parent=%s\n",
            index, win->qws_id,
            win->xdg_toplevel ? "toplevel" : "child",
            win->name    ? win->name    : "",
            win->caption ? win->caption : "",
            (unsigned)win->win_flags,
            win->win_flags != (qws_window_flags_t)-1
                ? qws_window_type_str((uint32_t)win->win_flags) : "?",
            g->x, g->y, g->width, g->height,
            qws_image_format_name(win->client_shm.format), win->client_shm.format,
            win->server_shm.fd, win->server_shm.size,
            win->wl_surface    ? "yes" : "no",
            win->wl_subsurface ? "yes" : "no",
            win->parent        ? "yes" : "no");

    if (win->parent)
        fprintf(stderr, "       parent qws_id=%d\n", win->parent->qws_id);
}

/* ================================================================
 * Client dump
 * ================================================================ */

static void dump_client(const qwswl_client_t *client)
{
    fprintf(stderr, "client %d  fd=%d  windows=%td\n",
            client->client_id, client->fd,
            qwswl_window_stack_t_count(&client->window_stack));
    int i = 0;
    for (c_each(it, qwswl_window_stack_t, client->window_stack))
        dump_window(*it.ref, i++);
}

/* ================================================================
 * Public API — __attribute__((used, noinline)) ensures GDB can call
 * these reliably even at -O2 (they will not be optimised away or
 * inlined into callers that never reach them).
 * ================================================================ */

__attribute__((used, noinline))
void qd_all(void)
{
    if (!g_debug_state) {
        fprintf(stderr, "qd: not initialised\n");
        return;
    }
    fprintf(stderr, "\n=== qwswl state dump  screen=%dx%d ===\n",
            g_debug_state->screen_width, g_debug_state->screen_height);
    for (c_each(it, qwswl_client_map_t, g_debug_state->client_map))
        dump_client(it.ref->second);
    fprintf(stderr, "==========================================\n\n");
}

__attribute__((used, noinline))
void qd_client(int client_id)
{
    if (!g_debug_state) {
        fprintf(stderr, "qd: not initialised\n");
        return;
    }
    qwswl_client_map_t_iter it =
        qwswl_client_map_t_find(&g_debug_state->client_map, (int32_t)client_id);
    if (it.ref == qwswl_client_map_t_end(&g_debug_state->client_map).ref) {
        fprintf(stderr, "qd: client %d not found\n", client_id);
        return;
    }
    dump_client(it.ref->second);
}

__attribute__((used, noinline))
void qd_win(int32_t qws_id)
{
    if (!g_debug_state) {
        fprintf(stderr, "qd: not initialised\n");
        return;
    }
    for (c_each(cit, qwswl_client_map_t, g_debug_state->client_map)) {
        qwswl_client_t *client = cit.ref->second;
        qwswl_window_map_t_iter wit =
            qwswl_window_map_t_find(&client->window_map, qws_id);
        if (wit.ref != qwswl_window_map_t_end(&client->window_map).ref) {
            dump_window(wit.ref->second, 0);
            return;
        }
    }
    fprintf(stderr, "qd: window %d not found\n", qws_id);
}
