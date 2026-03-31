/*
 * trace_proxy.c - QWS man-in-the-middle trace proxy implementation
 * SPDX-License-Identifier: MIT
 */

#include "trace_proxy.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>

#define READ_BUF_SIZE 65536

/* Defined in main.c; set to 1 by the signal handler before listen_fd is closed. */
extern volatile sig_atomic_t g_interrupted;

/* ------------------------------------------------------------------ */
/* Write helper                                                         */
/* ------------------------------------------------------------------ */

/* Write all `len` bytes to fd, handling EINTR. Returns 0 on success. */
static int write_all(int fd, const void *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, (const uint8_t *)buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trace queue                                                          */
/* ------------------------------------------------------------------ */

static int trace_queue_init(trace_queue_t *tq)
{
    tq->head = NULL;
    tq->tail = NULL;
    if (pthread_mutex_init(&tq->lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&tq->cond, NULL) != 0) {
        pthread_mutex_destroy(&tq->lock);
        return -1;
    }
    return 0;
}

static void trace_queue_destroy(trace_queue_t *tq)
{
    /* Drain any remaining items (should only happen on abnormal exit). */
    trace_item_t *item = tq->head;
    while (item) {
        trace_item_t *next = item->next;
        if (item->pkt)
            qws_packet_free(item->pkt);
        free(item);
        item = next;
    }
    pthread_cond_destroy(&tq->cond);
    pthread_mutex_destroy(&tq->lock);
}

/* Enqueue one packet; ownership of pkt transfers into the queue.
 * Pass pkt=NULL to enqueue the shutdown sentinel. */
static void trace_enqueue(trace_queue_t *tq, qws_packet_t *pkt,
                           int32_t client_id, bool outgoing)
{
    trace_item_t *item = malloc(sizeof(*item));
    if (!item) {
        /* OOM: drop rather than crash */
        if (pkt)
            qws_packet_free(pkt);
        return;
    }
    item->pkt       = pkt;
    item->client_id = client_id;
    item->outgoing  = outgoing;
    item->next      = NULL;

    pthread_mutex_lock(&tq->lock);
    if (tq->tail)
        tq->tail->next = item;
    else
        tq->head = item;
    tq->tail = item;
    pthread_cond_signal(&tq->cond);
    pthread_mutex_unlock(&tq->lock);
}

/* ------------------------------------------------------------------ */
/* Tracer thread                                                        */
/* ------------------------------------------------------------------ */

static void *tracer_thread_fn(void *arg)
{
    trace_queue_t *tq = (trace_queue_t *)arg;

    for (;;) {
        pthread_mutex_lock(&tq->lock);
        while (tq->head == NULL)
            pthread_cond_wait(&tq->cond, &tq->lock);
        trace_item_t *item = tq->head;
        tq->head = item->next;
        if (tq->head == NULL)
            tq->tail = NULL;
        pthread_mutex_unlock(&tq->lock);

        if (item->pkt == NULL) {
            /* Shutdown sentinel — drain complete, exit. */
            free(item);
            break;
        }

        qws_trace_packet(item->client_id, item->pkt, item->outgoing);
        qws_packet_free(item->pkt);
        free(item);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-direction parsing → enqueue                                     */
/* ------------------------------------------------------------------ */

/* Parse `len` bytes from `buf` through `reader`; enqueue each complete
 * packet into `tq`.  Ownership of every parsed pkt transfers to tq. */
static void feed_and_enqueue(qws_reader_t *reader, const uint8_t *buf,
                              size_t len, int32_t client_id, bool outgoing,
                              trace_queue_t *tq)
{
    size_t offset = 0;
    while (offset < len) {
        qws_packet_t *pkt = NULL;
        size_t consumed = qws_reader_feed(reader, buf + offset,
                                          len - offset, &pkt);
        assert(consumed != 0 || pkt != NULL);
        offset += consumed;
        if (pkt)
            trace_enqueue(tq, pkt, client_id, outgoing);
    }
}

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* Called by either direction thread when it detects a disconnect.
 * The first caller (atomic 0→1) closes both fds; the second is a no-op. */
static void session_close_fds(qwstrace_session_t *sess, const char *who)
{
    int was_dead = atomic_exchange(&sess->dead, 1);
    if (was_dead)
        return;

    fprintf(stderr, "--- session %d: %s disconnected ---\n",
            sess->client_id, who);

    pthread_mutex_lock(&sess->close_mutex);
    if (sess->client_fd >= 0) {
        close(sess->client_fd);
        sess->client_fd = -1;
    }
    if (sess->server_fd >= 0) {
        close(sess->server_fd);
        sess->server_fd = -1;
    }
    pthread_mutex_unlock(&sess->close_mutex);
}

/* ------------------------------------------------------------------ */
/* Direction thread argument                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    qwstrace_session_t *sess;
    bool is_client_dir; /* true = read client_fd, forward to server_fd */
} dir_thread_arg_t;

/* ------------------------------------------------------------------ */
/* Client-direction thread: client_fd → server_fd                      */
/* ------------------------------------------------------------------ */

static void *client_dir_thread_fn(void *arg)
{
    dir_thread_arg_t   *a    = (dir_thread_arg_t *)arg;
    qwstrace_session_t *sess = a->sess;
    free(a);

    uint8_t buf[READ_BUF_SIZE];

    while (!atomic_load(&sess->dead)) {
        ssize_t n = read(sess->client_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            session_close_fds(sess, "client");
            break;
        }
        if (n == 0) {
            session_close_fds(sess, "client");
            break;
        }

        /* Forward verbatim to server */
        pthread_mutex_lock(&sess->close_mutex);
        int sfd = sess->server_fd;
        pthread_mutex_unlock(&sess->close_mutex);
        if (sfd >= 0 && write_all(sfd, buf, (size_t)n) < 0)
            session_close_fds(sess, "client");

        /* Parse and enqueue for tracing */
        feed_and_enqueue(&sess->cmd_reader, buf, (size_t)n,
                         sess->client_id, false, sess->tq);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Server-direction thread: server_fd → client_fd                      */
/* ------------------------------------------------------------------ */

static void *server_dir_thread_fn(void *arg)
{
    dir_thread_arg_t   *a    = (dir_thread_arg_t *)arg;
    qwstrace_session_t *sess = a->sess;
    free(a);

    uint8_t buf[READ_BUF_SIZE];

    while (!atomic_load(&sess->dead)) {
        ssize_t n = read(sess->server_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            session_close_fds(sess, "server");
            break;
        }
        if (n == 0) {
            session_close_fds(sess, "server");
            break;
        }

        /* Forward verbatim to client */
        pthread_mutex_lock(&sess->close_mutex);
        int cfd = sess->client_fd;
        pthread_mutex_unlock(&sess->close_mutex);
        if (cfd >= 0 && write_all(cfd, buf, (size_t)n) < 0)
            session_close_fds(sess, "server");

        /* Parse and enqueue for tracing */
        feed_and_enqueue(&sess->evt_reader, buf, (size_t)n,
                         sess->client_id, true, sess->tq);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Session helpers                                                      */
/* ------------------------------------------------------------------ */

static qwstrace_session_t *session_create(int cfd, int sfd, int client_id,
                                           trace_queue_t *tq)
{
    qwstrace_session_t *sess = calloc(1, sizeof(*sess));
    if (!sess)
        return NULL;

    sess->client_fd = cfd;
    sess->server_fd = sfd;
    sess->client_id = client_id;
    sess->tq        = tq;
    atomic_init(&sess->dead, 0);
    pthread_mutex_init(&sess->close_mutex, NULL);
    qws_reader_init(&sess->cmd_reader, true  /* reading commands */);
    qws_reader_init(&sess->evt_reader, false /* reading events  */);
    return sess;
}

static void session_start_threads(qwstrace_session_t *sess)
{
    dir_thread_arg_t *a1 = malloc(sizeof(*a1));
    a1->sess = sess;
    a1->is_client_dir = true;
    pthread_create(&sess->client_thread, NULL, client_dir_thread_fn, a1);

    dir_thread_arg_t *a2 = malloc(sizeof(*a2));
    a2->sess = sess;
    a2->is_client_dir = false;
    pthread_create(&sess->server_thread, NULL, server_dir_thread_fn, a2);

    sess->threads_started = true;
}

/* Stop both direction threads and free the session. */
static void session_stop_and_free(qwstrace_session_t *sess)
{
    /* Close fds to unblock blocking read() calls in both threads. */
    session_close_fds(sess, "session-drop");

    if (sess->threads_started) {
        pthread_join(sess->client_thread, NULL);
        pthread_join(sess->server_thread, NULL);
    }

    /* Safe to reset readers now that both threads have exited. */
    qws_reader_reset(&sess->cmd_reader);
    qws_reader_reset(&sess->evt_reader);
    pthread_mutex_destroy(&sess->close_mutex);
    free(sess);
}

/* ------------------------------------------------------------------ */
/* Wait for upstream server                                            */
/* ------------------------------------------------------------------ */

static void wait_until_server_ready(qwstrace_state_t *st)
{
    int sfd;
    while ((sfd = qws_client_connect(st->upstream_paths.socket)) < 0) {
        if (g_interrupted)
            exit(1);
        fprintf(stderr, "qwstrace: upstream %s not yet available: %s\n",
                st->upstream_paths.socket, strerror(errno));
        nanosleep(&(struct timespec){0, 200000000}, NULL); /* 200 ms */
    }

    fprintf(stderr, "qwstrace: upstream %s is now available\n",
            st->upstream_paths.socket);
    close(sfd);
}

/* ------------------------------------------------------------------ */
/* Init / Run / Shutdown                                               */
/* ------------------------------------------------------------------ */

int qwstrace_init(qwstrace_state_t *st, int listen_display, int upstream_display)
{
    memset(st, 0, sizeof(*st));
    st->listen_fd = -1;

    if (qws_init_display_dir(listen_display, &st->listen_paths) != 0) {
        fprintf(stderr, "qwstrace: failed to initialise display directory for :%d\n",
                listen_display);
        return -1;
    }
    if (qws_display_paths_fill(upstream_display, &st->upstream_paths) != 0) {
        fprintf(stderr, "qwstrace: failed to construct paths for upstream display :%d\n",
                upstream_display);
        return -1;
    }

    if (symlink(st->upstream_paths.fontdb, st->listen_paths.fontdb) != 0) {
        fprintf(stderr, "qwstrace: failed to symlink fontdb %s -> %s: %s\n",
                st->listen_paths.fontdb, st->upstream_paths.fontdb, strerror(errno));
        return -1;
    }

    wait_until_server_ready(st);

    st->listen_fd = qws_server_listen(st->listen_paths.socket);
    if (st->listen_fd < 0) {
        fprintf(stderr, "qwstrace: failed to listen on %s: %s\n",
                st->listen_paths.socket, strerror(errno));
        return -1;
    }

    if (qws_shm_create(&st->listen_display_shm, QWS_DISPLAY_SHM_SIZE) != 0) {
        fprintf(stderr, "[qwswayland] Failed to create display shm\n");
        return -1;
    }
    st->listen_display_lock = qlock_create(st->listen_paths.socket, 'd');
    if (st->listen_display_lock == NULL) {
        fprintf(stderr, "[qwswayland] Failed to create display lock\n");
        return -1;
    }

    st->running = true;
    fprintf(stderr, "qws_trace_proxy: listening on %s\n", st->listen_paths.socket);
    fprintf(stderr, "qws_trace_proxy: will connect upstream to %s\n",
            st->upstream_paths.socket);
    return 0;
}

/* ------------------------------------------------------------------ */

int qwstrace_run(qwstrace_state_t *st)
{
    if (trace_queue_init(&st->trace_queue) != 0) {
        fprintf(stderr, "qwstrace: failed to init trace queue\n");
        return -1;
    }
    if (pthread_create(&st->tracer_thread, NULL,
                       tracer_thread_fn, &st->trace_queue) != 0) {
        fprintf(stderr, "qwstrace: failed to start tracer thread\n");
        trace_queue_destroy(&st->trace_queue);
        return -1;
    }
    st->tracer_started = true;

    while (st->running) {
        /* Blocking accept — unblocked by signal handler closing listen_fd. */
        int cfd = qws_server_accept(st->listen_fd);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            /* EBADF: listen_fd closed by signal handler → clean exit. */
            break;
        }

        /* Drop any existing session before starting the new one. */
        if (st->session) {
            session_stop_and_free(st->session);
            st->session = NULL;
        }

        st->client_id++;
        fprintf(stderr, "\n--- session %d: client connected ---\n",
                st->client_id);

        int sfd = qws_client_connect(st->upstream_paths.socket);
        if (sfd < 0) {
            fprintf(stderr, "qwstrace: cannot connect to upstream %s: %s\n",
                    st->upstream_paths.socket, strerror(errno));
            close(cfd);
            continue;
        }
        fprintf(stderr, "--- session %d: upstream connected ---\n\n",
                st->client_id);

        st->session = session_create(cfd, sfd, st->client_id, &st->trace_queue);
        if (!st->session) {
            fprintf(stderr, "qwstrace: out of memory creating session\n");
            close(cfd);
            close(sfd);
            continue;
        }

        session_start_threads(st->session);
    }

    /* Tear down the active session (if any). */
    if (st->session) {
        session_stop_and_free(st->session);
        st->session = NULL;
    }

    /* Signal tracer to drain and exit, then join it. */
    trace_enqueue(&st->trace_queue, NULL, 0, false);
    pthread_join(st->tracer_thread, NULL);
    st->tracer_started = false;

    return 0;
}

/* ------------------------------------------------------------------ */

void qwstrace_shutdown(qwstrace_state_t *st)
{
    /* session and tracer are already cleaned up by qwstrace_run(); guard
     * defensively in case init succeeded but run was never called. */
    if (st->session) {
        session_stop_and_free(st->session);
        st->session = NULL;
    }
    if (st->tracer_started) {
        trace_enqueue(&st->trace_queue, NULL, 0, false);
        pthread_join(st->tracer_thread, NULL);
        st->tracer_started = false;
    }
    trace_queue_destroy(&st->trace_queue);

    qws_pcap_writer_close(st->pcap_writer);
    st->pcap_writer = NULL;

    qws_shm_destroy(&st->listen_display_shm);

    if (st->listen_display_lock != NULL)
        qlock_destroy(st->listen_display_lock);

    if (st->listen_fd >= 0) {
        close(st->listen_fd);
        st->listen_fd = -1;
    }

    st->running = false;
}
