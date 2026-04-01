/*
 * proxy.h - QWSWayland proxy daemon command dispatch
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_H
#define PROXY_H

#include "client.h"
#include "lifecycle.h"
#include "qws_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read and process pending data from a QWS client socket. */
void qwswl_handle_client_data(qwswl_state_t *state, qwswl_client_t *client);

/* Dispatch a single decoded QWS command packet to the appropriate handler. */
void qwswl_dispatch_command(qwswl_state_t *state, qwswl_client_t *client,
                            qws_packet_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* PROXY_H */
