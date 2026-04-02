

/*
 * client.c - QWSWayland proxy daemon client management
 * SPDX-License-Identifier: MIT
 */

#include "client.h"
#include "qws_rect.h"
#include "qws_trace.h"
#include "window.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <wayland-client.h>

#include <assert.h>

#define T qwswl_window_map_t, int32_t, qwswl_window_t *
#define i_declared
#include "stc/hashmap.h"

#define T qwswl_window_stack_t, qwswl_window_t *
#define i_declared
#include "stc/list.h"

/* ================================================================
 * Client management
 * ================================================================ */

int32_t qwswl_allocate_ids(qwswl_client_t *client, int32_t count) {
    assert(client && count > 0);
    int32_t first_id = client->next_window_id + 1;
    client->next_window_id += count;
    return first_id;
}

qwswl_client_t *qwswl_create_client(int fd, int32_t id) {
    qwswl_client_t *cl = calloc(1, sizeof(*cl));
    assert(cl != NULL);
    cl->fd = fd;
    cl->client_id = id;
    cl->next_window_id = cl->client_id * 1000;
    qws_reader_init(&cl->reader, true);
    qwswl_window_map_t_init();

    return cl;
}

void qwswl_destroy_client(qwswl_state_t *state, qwswl_client_t *cl) {
    /* Destroy all windows belonging to this client */
    for (c_each_kv(qws_id, win, qwswl_window_map_t, cl->window_map)) {
        qwswl_destroy_window(state, *win);
    }

    qwswl_window_stack_t_drop(&cl->window_stack);

    close(cl->fd);

    /* Destroy the client's lock - that should get it to start
     * getting an idea about what is about to happen */
    qwslock_destroy(cl->lock, true);

    free(cl);
}

/* The window stack is split into two logical zones (mirrors Qt4 QWS):
 *
 *   [ front/top ]  on-top windows  |  normal windows  [ back/bottom ]
 *
 * on_top windows always live in the front zone; normal windows behind them.
 * New windows are inserted at the boundary — after all on_top windows. */
static void push_window_to_stack(qwswl_client_t *client, qwswl_window_t *win) {
    /* Insert before the first non-on_top window (= after the on_top zone). */
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (!(*it.ref)->on_top) {
            qwswl_window_stack_t_insert_at(&client->window_stack, it, win);
            return;
        }
    }
    /* Stack is empty or all windows are on_top: append. */
    qwswl_window_stack_t_push_back(&client->window_stack, win);
}

/* Mirrors Qt QWSServerPrivate::findWindow(id, client):
 * if the window is unknown, lazily allocate it and place it on the stack. */
qwswl_window_t *qwswl_find_or_allocate_window(qwswl_client_t *client,
                                              int32_t qws_id) {
    assert(client && qws_id > 0);
    qwswl_window_map_t_iter p =
        qwswl_window_map_t_find(&client->window_map, qws_id);
    if (p.ref) {
        assert(p.ref);
        return p.ref->second;
    }
    qwswl_window_t *win = qwswl_allocate_window_with_id(client, qws_id);
    push_window_to_stack(client, win);
    return win;
}

void qwswl_add_window_to_client(qwswl_client_t *client, qwswl_window_t *win) {
    assert(client && win && win->qws_id > 0);
    qwswl_window_map_t_result result =
        qwswl_window_map_t_insert(&client->window_map, win->qws_id, win);
    assert(result.inserted);
}

void qwswl_remove_window_from_client(qwswl_client_t *client, int32_t qws_id) {
    assert(client && qws_id > 0);
    qwswl_window_map_t_iter mit =
        qwswl_window_map_t_find(&client->window_map, qws_id);
    if (!mit.ref)
        return;

    qwswl_window_t *win = mit.ref->second;
    qwswl_window_map_t_erase_at(&client->window_map, mit);

    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_erase_at(&client->window_stack, it);
            break;
        }
    }
}

qwswl_window_t *
qwswl_client_window_get_first_toplevel_below(qwswl_client_t *client,
                                             qwswl_window_t *win) {
    bool next = false;
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        qwswl_window_t *it_win = *it.ref;
        if (next && qwswl_win_is_toplevel(it_win))
            return *it.ref;
        if (*it.ref == win)
            next = true;
    }
    return NULL;
}

/* Raise win to the top of its zone (mirrors Qt QWSServerPrivate::raiseWindow):
 *   on_top windows  → absolute front of the stack
 *   normal windows  → first position after the on_top zone */
void qwswl_stack_raise_window(qwswl_client_t *client, qwswl_window_t *win) {
    /* Remove from current position. */
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_erase_at(&client->window_stack, it);
            break;
        }
    }
    if (win->on_top) {
        qwswl_window_stack_t_push_front(&client->window_stack, win);
        return;
    }
    /* Normal window: insert before the first non-on_top window. */
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (!(*it.ref)->on_top) {
            qwswl_window_stack_t_insert_at(&client->window_stack, it, win);
            return;
        }
    }
    qwswl_window_stack_t_push_back(&client->window_stack, win);
}

/* Lower win to the absolute back of the stack (mirrors Qt lowerWindow).
 * Note: this is a full move-to-bottom, not a one-step swap. */
void qwswl_stack_lower_window(qwswl_client_t *client, qwswl_window_t *win) {
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        if (*it.ref == win) {
            qwswl_window_stack_t_erase_at(&client->window_stack, it);
            break;
        }
    }
    qwswl_window_stack_t_push_back(&client->window_stack, win);
}

/* Re-establish Wayland subsurface Z-order for all children of parent to match
 * the logical window stack.  Must be called after any raise/lower operation.
 *
 * Wayland subsurface ordering uses wl_subsurface_place_above(sub, sibling),
 * which positions sub just above sibling in the parent's composite order.
 * We build the chain from the bottom up:
 *
 *   children[n-1] placed above parent->wl_surface  (bottommost sibling)
 *   children[n-2] placed above children[n-1]
 *   ...
 *   children[0]   placed above children[1]          (topmost sibling)
 */
#define WINDOW_MAX_CHILDREN 128
void qwswl_reorder_subsurfaces(qwswl_client_t *cl, qwswl_window_t *parent) {
    if (!parent || !parent->wl_surface)
        return;

    /* Collect children of parent in stack order (topmost first). */
    qwswl_window_t *children[WINDOW_MAX_CHILDREN];
    int n = 0;
    for (c_each(it, qwswl_window_stack_t, cl->window_stack)) {
        qwswl_window_t *w = *it.ref;
        assert(n < WINDOW_MAX_CHILDREN);
        if (w->parent == parent && w->wl_subsurface)
            children[n++] = w;
    }
    if (n == 0)
        return;

    struct wl_surface *below = parent->wl_surface;
    for (int i = n - 1; i >= 0; i--) {
        wl_subsurface_place_above(children[i]->wl_subsurface, below);
        below = children[i]->wl_surface;
    }
}

/* Re-establish Z-order for all Qt shell toplevel windows to match the logical
 * window stack.  Mirrors qwswl_reorder_subsurfaces: collect toplevels in
 * stack order (topmost first), then raise them bottom-to-top so the logically
 * topmost window is raised last and ends up at the compositor front. */
void qwswl_reorder_toplevels(qwswl_client_t *cl) {
    qwswl_window_t *toplevels[WINDOW_MAX_CHILDREN];
    int n = 0;
    for (c_each(it, qwswl_window_stack_t, cl->window_stack)) {
        qwswl_window_t *w = *it.ref;
        if (w->zqt_shell_surface) {
            assert(n < WINDOW_MAX_CHILDREN);
            toplevels[n++] = w;
        }
    }
    for (int i = n - 1; i >= 0; i--)
        zqt_shell_surface_v1_raise(toplevels[i]->zqt_shell_surface);
}

void qwswl_stack_dump(const qwswl_client_t *client) {
    QWS_TRACE("client %d stack (%td windows):", client->client_id,
              qwswl_window_stack_t_count(&client->window_stack));
    int i = 0;
    for (c_each(it, qwswl_window_stack_t, client->window_stack)) {
        const qwswl_window_t *win = *it.ref;
        QWS_TRACE("  [%d] qws_id=%-4d flags=0x%08x%s%s", i++, win->qws_id,
                  (unsigned)win->win_flags,
                  win->win_flags != -1 && QWS_IS_TOPLEVEL_TYPE(win->win_flags)
                      ? " (toplevel)"
                      : "",
                  win->on_top ? " ON_TOP" : "");
    }
}

void qwswl_update_regions(qwswl_state_t *state, qwswl_client_t *cl,
                          qwswl_window_t *requesting_win,
                          qwswl_region_event_cb_t emit) {
    /* Accumulate opaque rects of all windows above the current one.
     * Built incrementally; grown with realloc. */
    qws_rect_t *blocking = NULL;
    int32_t n_blocking = 0;
    bool event_for_requesting_win_sent;

    for (c_each(it, qwswl_window_stack_t, cl->window_stack)) {
        qwswl_window_t *win = *it.ref;

        /* Compute the new allocation for this window. */
        qws_rect_t *new_alloc = NULL;
        int32_t n_new = 0;

        if (win->geometry.nrects > 0) {
            for (int32_t i = 0; i < win->geometry.nrects; i++) {
                int32_t n_sub = 0;
                qws_rect_t *sub = qws_rect_subtract(
                    &win->geometry.rects[i], 1, blocking, n_blocking, &n_sub);
                if (n_sub > 0) {
                    new_alloc = realloc(new_alloc, (size_t)(n_new + n_sub) *
                                                       sizeof(qws_rect_t));
                    memcpy(new_alloc + n_new, sub,
                           (size_t)n_sub * sizeof(qws_rect_t));
                    n_new += n_sub;
                }
                free(sub);
            }
        }

        qws_clip_rects(new_alloc, n_new, state->screen_width - 1,
                       state->screen_height - 1);

        /* The requesting window might need to get always an event if the client
         * has already reset its clip region to empty and will not try to
         * display anything without an explicit ack. All other windows only get
         * an event when the allocation changed. */
        bool force = (win == requesting_win);
        bool changed =
            (n_new != win->visible_nrects) ||
            (n_new > 0 && memcmp(new_alloc, win->visible_rects,
                                 (size_t)n_new * sizeof(qws_rect_t)) != 0);
        if (force)
            event_for_requesting_win_sent = true;

        if (force || changed) {
            free(win->visible_rects);
            win->visible_rects = new_alloc;
            win->visible_nrects = n_new;
            emit(cl, win, new_alloc, n_new);
        } else {
            free(new_alloc);
        }

        /* Opaque windows occlude everything below them. */
        if (win->opacity == 255 && n_new > 0) {
            qws_rect_t *tmp =
                realloc(blocking, (size_t)(n_blocking + win->visible_nrects) *
                                      sizeof(qws_rect_t));
            if (tmp) {
                blocking = tmp;
                memcpy(blocking + n_blocking, win->visible_rects,
                       (size_t)win->visible_nrects * sizeof(qws_rect_t));
                n_blocking += win->visible_nrects;
            }
        }
    }

    if (requesting_win && !event_for_requesting_win_sent)
        emit(cl, requesting_win, requesting_win->visible_rects,
             requesting_win->visible_nrects);

    free(blocking);
}