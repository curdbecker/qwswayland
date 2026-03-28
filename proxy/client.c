

/*
 * client.c - QWSWayland proxy daemon client management
 * SPDX-License-Identifier: MIT
 */

#include "client.h"
#include "window.h"
#include "qws_trace.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

#include <wayland-client.h>

#include <assert.h>

#define T qwswl_window_map_t, int32_t, qwswl_window_t*
#define i_declared
#include "stc/hashmap.h"

#define T qwswl_window_stack_t, qwswl_window_t*
#define i_declared
#include "stc/list.h"

/* ================================================================
 * Client management
 * ================================================================ */

qwswl_client_t *qwswl_create_client(int fd, int32_t id)
{
    qwswl_client_t *cl = calloc(1, sizeof(*cl));
    assert(cl != NULL);
    cl->fd = fd;
    cl->client_id = id;
    cl->focused_window_id = -1;
    cl->next_window_id = cl->client_id * 1000;
    qws_reader_init(&cl->reader, true);
    qwswl_window_map_t_init();

    return cl;
}

void qwswl_destroy_client(qwswl_state_t *state, qwswl_client_t *cl)
{
    /* Destroy all windows belonging to this client */
    for(c_each_kv(qws_id, win, qwswl_window_map_t, cl->window_map)) {
        qwswl_destroy_window(state, *win);
    }

    qwswl_window_stack_t_drop(&cl->window_stack);

    /* Detach from client's lock (we don't own it, client does) */
    qwslock_destroy(cl->lock);
    cl->lock = NULL;

    close(cl->fd);
    free(cl);
}

qwswl_window_t *
qwswl_lookup_window_on_client(qwswl_client_t *client, int32_t qws_id)
{
    assert(client && qws_id > 0);
    qwswl_window_t * const *win = 
        qwswl_window_map_t_at(&client->window_map, qws_id);
    if (win) {
        assert(*win);
        return *win;
    }
    return NULL;
}

void qwswl_add_window_to_client(qwswl_client_t *client,
    int32_t qws_id, qwswl_window_t *win)
{
    assert(client && win && qws_id > 0);
    qwswl_window_map_t_result result =
        qwswl_window_map_t_insert(&client->window_map, qws_id, win);
    assert(result.inserted);
    qwswl_window_stack_t_push_back(&client->window_stack, win);
}

void qwswl_remove_window_from_client(qwswl_client_t *client,
    int32_t qws_id)
{
    assert(client && qws_id > 0);
    qwswl_window_map_t_iter mit = qwswl_window_map_t_find(&client->window_map, qws_id);
    assert(mit.ref);

    qwswl_window_t *win = mit.ref->second;
    qwswl_window_map_t_erase_at(&client->window_map, mit);

    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_erase_at(&client->window_stack, it);
            break;
        }
    }
}

qwswl_window_t *qwswl_find_top_toplevel_in_stack(qwswl_client_t *client)
{
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        qwswl_window_t *win = *it.ref;
        if (win->win_flags != -1 && QWS_IS_TOPLEVEL_TYPE(win->win_flags))
            return win;
    }
    return NULL;
}

qwswl_window_t *qwswl_find_active_top_in_stack(qwswl_client_t *client)
{
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        qwswl_window_t *win = *it.ref;
        if (win->win_flags != -1 && win->geometry.height > 0 
            && win->geometry.width > 0)
            return win;
    }
    return NULL;
}

void qwswl_stack_move_to_top(qwswl_client_t *client, qwswl_window_t *win)
{
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_erase_at(&client->window_stack, it);
            break;
        }
    }
    qwswl_window_stack_t_push_front(&client->window_stack, win);
}

void qwswl_stack_move_up(qwswl_client_t *client, qwswl_window_t *win)
{
    /* "up" = toward front (higher z-order); swap with predecessor */
    qwswl_window_t **prev_ref = NULL;
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            if (!prev_ref) return;   /* already at top (front) */
            qwswl_window_t *tmp = *it.ref;
            *it.ref   = *prev_ref;
            *prev_ref = tmp;
            return;
        }
        prev_ref = it.ref;
    }
}

void qwswl_stack_move_down(qwswl_client_t *client, qwswl_window_t *win)
{
    /* "down" = toward back (lower z-order); swap with successor */
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_iter next = it;
            qwswl_window_stack_t_next(&next);
            if (!next.ref) return;   /* already at bottom (back) */
            qwswl_window_t *tmp = *it.ref;
            *it.ref   = *next.ref;
            *next.ref = tmp;
            return;
        }
    }
}

void qwswl_stack_dump(const qwswl_client_t *client)
{
    QWS_TRACE("client %d stack (%td windows):",
              client->client_id,
              qwswl_window_stack_t_count(&client->window_stack));
    int i = 0;
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        const qwswl_window_t *win = *it.ref;
        QWS_TRACE("  [%d] qws_id=%-4d flags=0x%08x%s",
                  i++, win->qws_id, (unsigned)win->win_flags,
                  win->win_flags != -1 && QWS_IS_TOPLEVEL_TYPE(win->win_flags)
                      ? " (toplevel)" : "");
    }
}

void qwswl_set_window_focus_on_client(qwswl_client_t *client,
    int32_t win_id, qws_focus_flag_t flag)
{
    switch (flag) {
    case QWS_FOCUS_GAIN:
        // assert(client->focused_window_id == -1 || 
            // client->focused_window_id == win_id);
        client->focused_window_id = win_id;
        break;
    case QWS_FOCUS_LOSE:
        // assert(client->focused_window_id == win_id);
        client->focused_window_id = -1;
        break;
    default:
        assert(false);
    }
}