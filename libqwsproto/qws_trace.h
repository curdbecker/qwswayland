/*
 * qws_trace.h - QWS protocol tracing and debug logging
 *
 * Provides detailed human-readable dumps of QWS packets including
 * decoded field values and hex dumps of raw payloads. Invaluable
 * for debugging wire format issues against real Qt 4.8.7 clients.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_TRACE_H
#define QWS_TRACE_H

#include "qws_proto.h"
#include "qws_pcap.h"

#include <stdint.h>
#include <stdio.h>

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QWS_TRACE(fmt, ...) \
    do { \
        fprintf(stderr, "[trace] %s: " fmt "\n", __func__, ##__VA_ARGS__); \
    } while (0)

/* -----------------------------------------------------------
 * Verbosity levels
 * ----------------------------------------------------------- */

enum qws_trace_level {
    QWS_TRACE_OFF      = 0,   /* no tracing */
    QWS_TRACE_BASIC    = 1,   /* one-line per packet: type + sizes */
    QWS_TRACE_FIELDS   = 2,   /* decode known struct fields */
    QWS_TRACE_HEXDUMP  = 3,   /* fields + hex dump of all data */
};

/* -----------------------------------------------------------
 * Global trace state
 * ----------------------------------------------------------- */

/* Set the trace level. Default is QWS_TRACE_OFF. */
void qws_trace_set_level(int level);

/* Attach a PCAP writer.  Every subsequent qws_trace_packet() call writes one
 * frame to the file regardless of the trace level or exclusion masks.
 * Pass NULL to detach.  Lifecycle (open/close) remains with the caller. */
void qws_trace_set_pcap_writer(qws_pcap_writer_t *w);

/* Get the current trace level. */
int  qws_trace_get_level(void);

/* Set the output FILE* for trace output. Default is stderr. */
void qws_trace_set_output(FILE *fp);

/* Set/get the exclusion mask.
 * cmd_mask: bitmask of command types to suppress (bit N = QWS_CMD_* type N).
 * evt_mask: bitmask of event types to suppress  (bit N = QWS_EVT_* type N). */
void qws_trace_set_exclude_mask(uint64_t cmd_mask, uint64_t evt_mask);
void qws_trace_get_exclude_mask(uint64_t *cmd_mask, uint64_t *evt_mask);

/* Parse a comma-separated exclude list into cmd/evt bitmasks.
 * Each token may be prefixed with "cmd:" or "evt:" to restrict the direction;
 * unprefixed tokens are matched against both.  OR's results into *cmd_mask
 * and *evt_mask (initialise them to 0 before the first call).
 * Prints a warning and calls exit(1) on an unrecognised name. */
void qws_trace_parse_exclude_list(const char *list,
                                   uint64_t *cmd_mask, uint64_t *evt_mask);

/* -----------------------------------------------------------
 * Packet tracing
 * ----------------------------------------------------------- */

void qws_trace_packet(const int32_t client_id, const qws_packet_t *pkt,
                      bool outgoing);

/* Log raw bytes on the wire (before/after parsing).
 * Useful for diagnosing framing issues. */
void qws_trace_raw_bytes(const int32_t client_id,
                          const void *data, size_t len);

/* -----------------------------------------------------------
 * Field-level decoders (used by qws_trace_packet internally,
 * but also available for custom logging)
 * ----------------------------------------------------------- */

/* Decode and print the simpleData fields of a command packet */
void qws_trace_decode_command(FILE *fp, int32_t type,
                               const void *simple_data, int32_t simple_len,
                               const void *raw_data, int32_t raw_len);

/* Decode and print the simpleData fields of an event packet */
void qws_trace_decode_event(FILE *fp, int32_t type,
                              const void *simple_data, int32_t simple_len,
                              const void *raw_data, int32_t raw_len);

/* Print a hex dump of `len` bytes at `data` with an indent prefix */
void qws_trace_hexdump(FILE *fp, const char *indent,
                         const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* QWS_TRACE_H */