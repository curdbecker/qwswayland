/*
 * client.c - QWSWayland proxy daemon client management
 * SPDX-License-Identifier: MIT
 */

#ifndef CLIENT_H
#define CLIENT_H

#include "lifecycle.h"
#include "window.h"
#include "qws_proto.h"
#include "qws_lock.h"

#include "stc/types.h"
#ifdef __cplusplus
extern "C" {
#endif

declare_hashmap(qwswl_window_map_t, int32_t, qwswl_window_t*);
declare_list(qwswl_window_stack_t, qwswl_window_t*);

/* -----------------------------------------------------------
 * Per-client state: one per connected QWS application
 * ----------------------------------------------------------- */
typedef struct qwswl_client {
    int                 fd;            /* unix socket fd to QWS client */
    int32_t             client_id;
    qws_reader_t        reader;        /* incremental packet parser */

    qwswl_window_map_t   window_map;
    qwswl_window_stack_t window_stack;  /* insertion-ordered list of windows */
    int32_t              next_window_id;

    int32_t             focused_window_id;

    /* Per-client lock (3 semaphores: BackingStore, Communication, RegionEvent) */
    qws_lock_t          lock;
} qwswl_client_t;

qwswl_client_t *qwswl_create_client(int fd, int32_t id);

void qwswl_destroy_client(qwswl_state_t *state, qwswl_client_t *client);

qwswl_window_t *qwswl_lookup_window_on_client(qwswl_client_t *client, int32_t qws_id);
void qwswl_add_window_to_client(qwswl_client_t *client, int32_t qws_id, qwswl_window_t *win);
void qwswl_remove_window_from_client(qwswl_client_t *client, int32_t qws_id);

qwswl_window_t *qwswl_find_top_toplevel_in_stack(qwswl_client_t *client);
qwswl_window_t *qwswl_find_active_top_in_stack(qwswl_client_t *client);

void qwswl_stack_move_to_top(qwswl_client_t *client, qwswl_window_t *win);
void qwswl_stack_move_up(qwswl_client_t *client, qwswl_window_t *win);
void qwswl_stack_move_down(qwswl_client_t *client, qwswl_window_t *win);
void qwswl_stack_dump(const qwswl_client_t *client);

void qwswl_set_window_focus_on_client(qwswl_client_t *client, int32_t win_id, qws_focus_flag_t flag);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_H */