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
#include <sys/epoll.h>

#define READ_BUF_SIZE 65536

/* ------------------------------------------------------------------ */

static void drop_connection(qwstrace_state_t *st)
{
    if (st->client_fd >= 0) {
        epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->client_fd, NULL);
        close(st->client_fd);
        st->client_fd = -1;
    }
    if (st->server_fd >= 0) {
        epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->server_fd, NULL);
        close(st->server_fd);
        st->server_fd = -1;
    }
    qws_reader_reset(&st->cmd_reader);
    qws_reader_reset(&st->evt_reader);
}

/* Feed `len` bytes from `buf` into `reader`, tracing every complete packet.
 * `outgoing`: true for commands (client→server), false for events (server→client). */
static void feed_and_trace(qws_reader_t *reader, const void *buf, size_t len,
                            int client_id, bool outgoing)
{
    size_t offset = 0;
    while (offset < len) {
        qws_packet_t *pkt = NULL;
        size_t consumed = qws_reader_feed(reader,
                                          (const uint8_t *)buf + offset,
                                          len - offset, &pkt);
        assert(consumed != 0 || pkt != NULL);
        offset += consumed;
        if (pkt) {
            qws_trace_packet(client_id, pkt, outgoing);
            qws_packet_free(pkt);
        }
    }
}

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

int qwstrace_init(qwstrace_state_t *st, const char *listen_path,
                  const char *server_path)
{
    memset(st, 0, sizeof(*st));
    st->client_fd = -1;
    st->server_fd = -1;
    st->listen_fd = -1;
    st->epoll_fd  = -1;

    strncpy(st->server_path, server_path, sizeof(st->server_path) - 1);

    qws_reader_init(&st->cmd_reader, true  /* reading commands */);
    qws_reader_init(&st->evt_reader, false /* reading events  */);

    st->epoll_fd = epoll_create1(0);
    if (st->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    st->listen_fd = qws_server_listen(listen_path);
    if (st->listen_fd < 0) {
        fprintf(stderr, "qwstrace: failed to listen on %s: %s\n",
                listen_path, strerror(errno));
        return -1;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = st->listen_fd };
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->listen_fd, &ev);

    st->running = true;
    fprintf(stderr, "qws_trace_proxy: listening on %s\n", listen_path);
    fprintf(stderr, "qws_trace_proxy: will connect upstream to %s\n", server_path);
    return 0;
}

/* ------------------------------------------------------------------ */

int qwstrace_run(qwstrace_state_t *st)
{
    static uint8_t buf[READ_BUF_SIZE];
    struct epoll_event events[8];

    while (st->running) {
        int n = epoll_wait(st->epoll_fd, events, 8, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            /* listen_fd was closed by signal handler */
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* ---- New client connection ---- */
            if (fd == st->listen_fd) {
                int cfd = qws_server_accept(st->listen_fd);
                if (cfd < 0) {
                    perror("qwstrace: accept");
                    continue;
                }

                /* Drop any previous half-open session */
                drop_connection(st);

                st->client_id++;
                fprintf(stderr, "\n--- session %d: client connected ---\n",
                        st->client_id);

                int sfd = qws_client_connect(st->server_path);
                if (sfd < 0) {
                    fprintf(stderr, "qwstrace: cannot connect to upstream %s: %s\n",
                            st->server_path, strerror(errno));
                    close(cfd);
                    continue;
                }
                fprintf(stderr, "--- session %d: upstream connected ---\n\n",
                        st->client_id);

                st->client_fd = cfd;
                st->server_fd = sfd;

                struct epoll_event evc = {
                    .events = EPOLLIN | EPOLLHUP | EPOLLERR,
                    .data.fd = cfd,
                };
                struct epoll_event evs = {
                    .events = EPOLLIN | EPOLLHUP | EPOLLERR,
                    .data.fd = sfd,
                };
                epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, cfd, &evc);
                epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, sfd, &evs);
                continue;
            }

            /* ---- Data from QWS client (commands → server) ---- */
            if (fd == st->client_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    fprintf(stderr, "--- session %d: client disconnected ---\n",
                            st->client_id);
                    drop_connection(st);
                    continue;
                }
                ssize_t recv = read(st->client_fd, buf, sizeof(buf));
                if (recv <= 0) {
                    fprintf(stderr, "--- session %d: client disconnected ---\n",
                            st->client_id);
                    drop_connection(st);
                    continue;
                }
                /* Forward verbatim, then trace */
                if (st->server_fd >= 0)
                    write_all(st->server_fd, buf, (size_t)recv);
                feed_and_trace(&st->cmd_reader, buf, (size_t)recv,
                               st->client_id, false);
                continue;
            }

            /* ---- Data from upstream QWS server (events → client) ---- */
            if (fd == st->server_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    fprintf(stderr, "--- session %d: server disconnected ---\n",
                            st->client_id);
                    drop_connection(st);
                    continue;
                }
                ssize_t recv = read(st->server_fd, buf, sizeof(buf));
                if (recv <= 0) {
                    fprintf(stderr, "--- session %d: server disconnected ---\n",
                            st->client_id);
                    drop_connection(st);
                    continue;
                }
                /* Forward verbatim, then trace */
                if (st->client_fd >= 0)
                    write_all(st->client_fd, buf, (size_t)recv);
                feed_and_trace(&st->evt_reader, buf, (size_t)recv,
                               st->client_id, true);
                continue;
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */

void qwstrace_shutdown(qwstrace_state_t *st)
{
    drop_connection(st);

    if (st->listen_fd >= 0) {
        close(st->listen_fd);
        st->listen_fd = -1;
    }
    if (st->epoll_fd >= 0) {
        close(st->epoll_fd);
        st->epoll_fd = -1;
    }
    st->running = false;
}
