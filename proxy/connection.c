

/*
 * connection.c - QWSWayland proxy daemon connection management
 * SPDX-License-Identifier: MIT
 */

#include "connection.h"
#include "lifecycle.h"
#include "window.h"
#include "proxy.h"
#include "qws_trace.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

#include <wayland-client.h>

/* ================================================================
 * Client management
 * ================================================================ */

void qwswl_accept_client(qwswl_state_t *state, int32_t id)
{
    int fd = qws_server_accept(state->qws_server_fd);
    if (fd < 0)
        return;

    qwswl_client_t *cl = calloc(1, sizeof(*cl));
    assert(cl != NULL);
    cl->fd = fd;
    cl->client_id = id;
    qws_reader_init(&cl->reader, true);

    /* Add to epoll */
    struct epoll_event ev = { .events = EPOLLIN,
                               .data.ptr = (void *)cl };
    epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    fprintf(stderr, "[qwswayland] Client connected (fd=%d)\n", fd);
    return;
}

void qwswl_disconnect_client(qwswl_state_t *state, qwswl_client_t *cl)
{
    fprintf(stderr, "[qwswayland] Client %d disconnected\n", cl->client_id);

    /* Destroy all windows belonging to this client */
    for (int i = 0; i < QWSWL_MAX_WINDOWS; i++) {
        qwswl_destroy_window(state, &cl->windows[i]);
    }

    /* Detach from client's lock (we don't own it, client does) */
    qws_lock_destroy(&cl->lock);

    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL);
    close(cl->fd);

    free(cl);
}

/* ================================================================
 * Main event loop
 * ================================================================ */

int qwswl_run(qwswl_state_t *state)
{
    /* dummy variables to provide pointers that allow to 
     * recognize epool events for the server and wayland socket fds */
    static const char socket_event, wayland_event;
    static int32_t next_client_id = 1;
    struct epoll_event events[32];
    int wl_fd = wl_display_get_fd(state->wl_display);

    /* Watch QWS server socket for new connections */
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.ptr = (void *) &socket_event};
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, 
            state->qws_server_fd, &ev);
    }

    /* Watch Wayland display fd for events */
    {
        struct epoll_event ev = { .events = EPOLLIN,
                                   .data.ptr = (void *) &wayland_event};
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev);
    }

    fprintf(stderr, "[qwswayland] Entering main loop\n");

    while (true) {
        /* Flush Wayland before blocking */
        while (wl_display_prepare_read(state->wl_display) != 0)
            wl_display_dispatch_pending(state->wl_display);
        wl_display_flush(state->wl_display);

        int nfds = epoll_wait(state->epoll_fd, events, 32, 100);

        if (nfds < 0 && errno != EINTR) {
            if (errno == EBADFD)
                /* we're being shut down */
                return 0;

            perror("[qwswayland] epoll_wait");
            return 1;
        }

        bool had_wayland = false;

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == (void *) &socket_event) {
                /* New QWS client connection */
                qwswl_accept_client(state, next_client_id);
                next_client_id++;
            } else if (events[i].data.ptr == (void *) &wayland_event) {
                /* Wayland events */
                had_wayland = true;
            } else {
                qwswl_client_t *cl = 
                    (qwswl_client_t *) events[i].data.ptr;
                qwswl_handle_client_data(state, cl);
            }
        }

        if (had_wayland) {
            wl_display_read_events(state->wl_display);
        } else {
            wl_display_cancel_read(state->wl_display);
        }
        wl_display_dispatch_pending(state->wl_display);
    }

    return 0;
}