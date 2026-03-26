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

#include "qws_proto.h"
#include "qws_lock.h"
#include "qws_pcap.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  listen_fd;           /* QWS server socket — accepts clients */
    int  epoll_fd;
    int  client_fd;           /* currently connected QWS client, or -1 */
    int  server_fd;           /* connection to upstream QWS server, or -1 */

    qws_reader_t cmd_reader;  /* command parser  (client → server) */
    qws_reader_t evt_reader;  /* event parser    (server → client) */

    qws_shm_t    listen_display_shm;
    qlock_t     *listen_display_lock;

    char upstream_path[256];    /* upstream QWS server socket path */
    int  client_id;           /* incremented each session, used in trace labels */

    qws_pcap_writer_t *pcap_writer;  /* NULL if -w not given */

    bool running;
} qwstrace_state_t;

/* Initialise state and create the listen socket on socket_path.
 * Returns 0 on success, -1 on error. */
int qwstrace_init(qwstrace_state_t *st, const char *listen_path,
                  const char *upstream_path);

/* Run the proxy event loop. Blocks until a signal closes listen_fd. */
int qwstrace_run(qwstrace_state_t *st);

/* Clean up all resources. */
void qwstrace_shutdown(qwstrace_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* TRACE_PROXY_H */
