/*
 * trace_proxy.h - QWS man-in-the-middle trace proxy
 *
 * Sits between a QWS client and a real QWS server, forwarding bytes
 * verbatim in both directions while tracing every parsed packet via
 * libqwsproto's trace infrastructure.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TRACE_PROXY_H
#define TRACE_PROXY_H

#include "qws_lock.h"
#include "qws_pcap.h"
#include "qws_proto.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Tracer thread message queue                                          */
/* ------------------------------------------------------------------ */

/* A single item in the tracer's FIFO. Ownership of pkt transfers into
 * the queue; the tracer thread is responsible for qws_packet_free(). */
typedef struct trace_item {
    qws_packet_t *pkt; /* NULL = shutdown sentinel */
    int32_t client_id;
    bool outgoing; /* false = cmd (client→server), true = evt */
    struct trace_item *next;
} trace_item_t;

/* Lock-protected unbounded FIFO, drained by the dedicated tracer thread. */
typedef struct {
    trace_item_t *head; /* oldest item — dequeue from here */
    trace_item_t *tail; /* newest item — enqueue here */
    pthread_mutex_t lock;
    pthread_cond_t cond; /* signalled when queue becomes non-empty */
} trace_queue_t;

/* ------------------------------------------------------------------ */
/* Per-session state                                                    */
/* ------------------------------------------------------------------ */

/* Allocated by the main thread when a new client connects, freed after
 * both direction threads have been joined. */
typedef struct {
    int client_fd; /* -1 after close */
    int server_fd; /* -1 after close */
    int client_id;

    /* Close coordination: first direction thread to detect disconnect
     * wins the atomic_exchange(dead, 1) and closes both fds under
     * close_mutex.  The loser finds dead==1 and exits its loop. */
    pthread_mutex_t close_mutex;
    atomic_int dead;

    qws_reader_t cmd_reader; /* exclusive to client-dir thread */
    qws_reader_t evt_reader; /* exclusive to server-dir thread */

    trace_queue_t *tq; /* points to qwstrace_state_t.trace_queue */

    pthread_t client_thread;
    pthread_t server_thread;
    bool threads_started;
} qwstrace_session_t;

/* ------------------------------------------------------------------ */
/* Top-level proxy state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int listen_fd; /* closed by signal handler to unblock accept() */

    qws_shm_t listen_display_shm;
    qlock_t *listen_display_lock;

    qws_display_paths_t listen_paths;   /* paths for the display we listen on */
    qws_display_paths_t upstream_paths; /* paths for the upstream QWS server */
    int client_id; /* incremented each session, used in trace labels */

    qws_pcap_writer_t *pcap_writer; /* NULL if -w not given */

    /* Active session — only one at a time; NULL between sessions. */
    qwstrace_session_t *session;

    /* Tracer thread and its queue (lifetime: qwstrace_run). */
    trace_queue_t trace_queue;
    pthread_t tracer_thread;
    bool tracer_started;

    bool running;
} qwstrace_state_t;

/* Initialise state: sets up display directories, creates the listen socket,
 * and fills listen_paths / upstream_paths.
 * Returns 0 on success, -1 on error. */
int qwstrace_init(qwstrace_state_t *st, int listen_display,
                  int upstream_display);

/* Run the proxy event loop. Blocks until a signal closes listen_fd. */
int qwstrace_run(qwstrace_state_t *st);

/* Clean up all resources. */
void qwstrace_shutdown(qwstrace_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* TRACE_PROXY_H */
