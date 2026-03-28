/*
 * debug.h - Zero-overhead window state inspection for the QWSWayland proxy
 * SPDX-License-Identifier: MIT
 *
 * Two ways to trigger a dump:
 *   1. From a paused GDB / VSCode debug console:
 *        -exec call qd_all()
 *        -exec call qd_client(1)
 *        -exec call qd_win(5)
 *      Output appears on the proxy's stderr terminal.
 *
 *   2. From a shell while the proxy is running:
 *        kill -USR1 $(pgrep qwswayland)
 */

#ifndef DEBUG_H
#define DEBUG_H

#include "lifecycle.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after qwswl_init() completes.
 * Stores the global state pointer used by all qd_*() functions and
 * installs the SIGUSR1 handler. */
void qwswl_debug_init(qwswl_state_t *state);

/* Dump everything: screen dimensions, all clients, all windows. */
void qd_all(void);

/* Dump all windows belonging to client <client_id>. */
void qd_client(int client_id);

/* Find and dump a single window by QWS id (searches all clients). */
void qd_win(int32_t qws_id);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_H */
