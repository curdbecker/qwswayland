/*
 * client.c - QWSWayland proxy daemon client management
 * SPDX-License-Identifier: MIT
 */

#ifndef CLIENT_H
#define CLIENT_H

#include "lifecycle.h"
#include "qws_lock.h"
#include "qws_proto.h"
#include "window.h"

#include "stc/types.h"
#ifdef __cplusplus
extern "C" {
#endif

declare_hashmap(qwswl_window_map_t, int32_t, qwswl_window_t *);
declare_list(qwswl_window_stack_t, qwswl_window_t *);

/* -----------------------------------------------------------
 * Per-client state: one per connected QWS application
 * ----------------------------------------------------------- */
typedef struct qwswl_client {
    int fd; /* unix socket fd to QWS client */
    int32_t client_id;
    qws_reader_t reader; /* incremental packet parser */

    qwswl_window_map_t window_map;
    qwswl_window_stack_t window_stack;
    int32_t next_window_id;

    /* Per-client lock (3 semaphores: BackingStore, Communication, RegionEvent)
     */
    qwslock_t *lock;
} qwswl_client_t;

qwswl_client_t *qwswl_create_client(int fd, int32_t id);

void qwswl_destroy_client(qwswl_state_t *state, qwswl_client_t *client);

/* Allocate count contiguous IDs, advancing next_window_id.
 * Returns the first ID in the allocated range. */
int32_t qwswl_allocate_ids(qwswl_client_t *client, int32_t count);

/* Find window by ID; if not in map, lazily allocate and push to stack.
 * Mirrors Qt QWSServerPrivate::findWindow(id, client). */
qwswl_window_t *qwswl_find_or_allocate_window(qwswl_client_t *client,
                                              int32_t qws_id);
void qwswl_add_window_to_client(qwswl_client_t *client, qwswl_window_t *win);
void qwswl_remove_window_from_client(qwswl_client_t *client, int32_t qws_id);

qwswl_window_t *
qwswl_client_window_get_first_toplevel_below(qwswl_client_t *client,
                                             qwswl_window_t *win);

/* Raise win to top of its zone (front if on_top, else after on_top block).
 * Qt: raiseWindow — used for both Raise and StaysOnTop altitudes. */
void qwswl_stack_raise_window(qwswl_client_t *client, qwswl_window_t *win);
/* Move win to the absolute back of the stack. Qt: lowerWindow. */
void qwswl_stack_lower_window(qwswl_client_t *client, qwswl_window_t *win);
void qwswl_stack_dump(const qwswl_client_t *client);

/* Re-establish Wayland subsurface Z-order for all children of parent
 * to match the logical window stack.  Call after any raise/lower. */
void qwswl_reorder_subsurfaces(qwswl_client_t *cl, qwswl_window_t *parent);

/* Callback type used by qwswl_update_regions to emit a
 * QWSRegionEvent::Allocation. The implementation (send_region_event in proxy.c)
 * is responsible for sending the packet and unlocking the client lock. */
typedef void (*qwswl_region_event_cb_t)(qwswl_client_t *cl, qwswl_window_t *win,
                                        qws_rect_t *rects, int32_t nrects);

/* Walk the client's window stack and emit a QWSRegionEvent::Allocation to every
 * window whose allocated region changed.  requesting_win, if non-NULL, always
 * receives an event even when its allocation is unchanged (the client has
 * already reset its clip region and is blocked waiting for the ack). */
void qwswl_update_regions(qwswl_state_t *state, qwswl_client_t *cl,
                          qwswl_window_t *requesting_win,
                          qwswl_region_event_cb_t emit);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_H */