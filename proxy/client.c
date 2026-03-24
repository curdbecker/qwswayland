

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

#define T qwswl_window_map_t, int32_t, qwswl_window_t*
#define i_declared
#include "stc/hashmap.h"

/* ================================================================
 * Client management
 * ================================================================ */

qwswl_client_t *qwswl_create_client(int fd, int32_t id)
{
    qwswl_client_t *cl = calloc(1, sizeof(*cl));
    assert(cl != NULL);
    cl->fd = fd;
    cl->client_id = id;
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

    /* Detach from client's lock (we don't own it, client does) */
    qws_lock_destroy(&cl->lock);

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
}

void qwswl_remove_window_from_client(qwswl_client_t *client, 
    int32_t qws_id)
{
    assert(client && qws_id > 0);
    assert(qwswl_window_map_t_erase(&client->window_map, qws_id));
}
