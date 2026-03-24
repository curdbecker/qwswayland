/*
 * client.c - QWSWayland proxy daemon client management
 * SPDX-License-Identifier: MIT
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include "lifecycle.h"
#include "window.h"
#include "qws_proto.h"
#include "qws_lock.h"

#include "stc/types.h"
#ifdef __cplusplus
extern "C" {
#endif

declare_hashmap(qwswl_window_map_t, int32_t, qwswl_window_t*);

/* -----------------------------------------------------------
 * Per-client state: one per connected QWS application
 * ----------------------------------------------------------- */
typedef struct qwswl_client {
    int                 fd;            /* unix socket fd to QWS client */
    int32_t             client_id;
    qws_reader_t        reader;        /* incremental packet parser */

    qwswl_window_map_t  window_map; 
    int32_t             next_window_id;

    /* Per-client lock (3 semaphores: BackingStore, Communication, RegionEvent) */
    qws_lock_t          lock;
} qwswl_client_t;

qwswl_client_t *qwswl_create_client(int fd, int32_t id);

void qwswl_destroy_client(qwswl_state_t *state, qwswl_client_t *client);

qwswl_window_t *qwswl_lookup_window_on_client(qwswl_client_t *client, int32_t qws_id);
void qwswl_add_window_to_client(qwswl_client_t *client, int32_t qws_id, qwswl_window_t *win);
void qwswl_remove_window_from_client(qwswl_client_t *client, int32_t qws_id);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_H */