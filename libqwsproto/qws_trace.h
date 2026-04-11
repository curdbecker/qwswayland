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

#include "qws_pcap.h"
#include "qws_proto.h"

#include <stdint.h>
#include <stdio.h>

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QWS_TRACE(fmt, ...)                                                    \
    do {                                                                       \
        fprintf(stderr, "[trace] %s: " fmt "\n", __func__, ##__VA_ARGS__);     \
    } while (0)

/* -----------------------------------------------------------
 * Verbosity levels
 * ----------------------------------------------------------- */

enum qws_trace_level {
    QWS_TRACE_OFF = 0,     /* no tracing */
    QWS_TRACE_BASIC = 1,   /* one-line per packet: type + sizes */
    QWS_TRACE_BRIEF = 2,   /* + key fields; bbox for rects; no window flags */
    QWS_TRACE_FIELDS = 3,  /* + full decoded fields (was level 2) */
    QWS_TRACE_HEXDUMP = 4, /* + hex dump of all data (was level 3) */
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
int qws_trace_get_level(void);

/* Set the output FILE* for trace output. Default is stderr. */
void qws_trace_set_output(FILE *fp);

/* Filter mask: a single uint64_t encoding both command and event types.
 * Commands occupy bits 0-31 (one bit per QWS_CMD_* value).
 * Events    occupy bits 32-63 (one bit per QWS_EVT_* value, shifted by 32).
 * A set bit means the corresponding packet type is allowed through.
 * Default is QWS_TRACE_MASK_ALL (everything passes). */
#define QWS_TRACE_CMD_BIT(type) (1ULL << (uint32_t)(type))
#define QWS_TRACE_EVT_BIT(type) (1ULL << ((uint32_t)(type) + 32))
#define QWS_TRACE_MASK_ALL (~0ULL)
#define QWS_TRACE_MASK_NONE (0ULL)

void qws_trace_set_filter_mask(uint64_t mask);
uint64_t qws_trace_get_filter_mask(void);

/* Parse a comma-separated type-name list into a bitmask (same syntax as
 * qws_trace_parse_exclude_list / qws_trace_parse_include_list).
 * CMD types occupy bits 0-31, EVT types bits 32-63.
 * The caller supplies a zeroed *out; bits for matched types are OR'd in.
 * Returns false and prints a warning on unrecognised names. */
bool qws_trace_parse_filter_mask(const char *list, uint64_t *out);

/* Parse a comma-separated list of packet type names.
 * Each token may be prefixed with "cmd:" or "evt:" to restrict the direction;
 * unprefixed tokens are matched against both.
 * Returns false and prints a warning on an unrecognised name.
 *
 * parse_exclude_list: clears the matching bits in the filter mask.
 * parse_include_list: resets the filter mask to NONE, then sets matching bits.
 */
bool qws_trace_parse_exclude_list(const char *list);
bool qws_trace_parse_include_list(const char *list);

/* Parse a trace level name and apply it via qws_trace_set_level().
 * Accepts "off", "basic", "brief", "fields", "hexdump", or "0"–"4".
 * Returns false on an unrecognised name. */
bool qws_trace_parse_level(const char *name);

/* -----------------------------------------------------------
 * Packet tracing
 * ----------------------------------------------------------- */

void qws_trace_packet(const int32_t client_id, const qws_packet_t *pkt,
                      bool outgoing);

/* Flags for qws_trace_packet_ex */
#define QWS_TRACE_PKT_DROPPED                                                  \
    0x1u /* packet was intercepted and not forwarded */

/* Like qws_trace_packet but accepts flags.  Dropped packets bypass the
 * filter mask (they are always logged) and are printed in red on a TTY. */
void qws_trace_packet_ex(int32_t client_id, const qws_packet_t *pkt,
                         bool outgoing, uint32_t flags);

/* Log raw bytes on the wire (before/after parsing).
 * Useful for diagnosing framing issues. */
void qws_trace_raw_bytes(const int32_t client_id, const void *data, size_t len);

/* -----------------------------------------------------------
 * Field-level decoders (used by qws_trace_packet internally,
 * but also available for custom logging)
 * ----------------------------------------------------------- */

/* Decode and print the simpleData fields of a command packet.
 * brief=true omits window flags and uses a bounding box for rect arrays. */
void qws_trace_decode_command(FILE *fp, int32_t type, const void *simple_data,
                              int32_t simple_len, const void *raw_data,
                              int32_t raw_len, bool brief);

/* Decode and print the simpleData fields of an event packet.
 * brief=true uses a bounding box for rect arrays. */
void qws_trace_decode_event(FILE *fp, int32_t type, const void *simple_data,
                            int32_t simple_len, const void *raw_data,
                            int32_t raw_len, bool brief);

/* Print a hex dump of `len` bytes at `data` with an indent prefix */
void qws_trace_hexdump(FILE *fp, const char *indent, const void *data,
                       size_t len);

/* Print an array of rects, one per line, to fp */
void qws_trace_print_rects(FILE *fp, const qws_rect_t *rects, int nr_rects);

/* Print the bounding box of an array of rects (single line) */
void qws_trace_print_rects_bbox(FILE *fp, const qws_rect_t *rects,
                                int nr_rects);

#ifdef __cplusplus
}
#endif

#endif /* QWS_TRACE_H */