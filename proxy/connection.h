/*
 * connection.h - QWSWayland proxy daemon connection management
 * SPDX-License-Identifier: MIT
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include "lifecycle.h"
#include "window.h"
#include "qws_proto.h"
#include "qws_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------
 * Per-client state: one per connected QWS application
 * ----------------------------------------------------------- */
typedef struct qwswl_state qwswl_state_t;

typedef struct qwswl_client {
    int              fd;            /* unix socket fd to QWS client */
    int32_t          client_id;
    qws_reader_t     reader;        /* incremental packet parser */

    qwswl_window_t   *windows[QWSWL_MAX_WINDOWS];
    int32_t          next_window_id;

    /* Per-client lock (3 semaphores: BackingStore, Communication, RegionEvent) */
    qws_lock_t       lock;
} qwswl_client_t;

/* Accept a new QWS client connection and register it in the proxy state. */
void qwswl_accept_client(qwswl_state_t *state, int32_t id);

/* Disconnect a client, cleaning up its windows and file descriptors. */
void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *client);

/* Run the main epoll event loop (blocks until shutdown). */
int  qwswl_run(qwswl_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_H */