/*
 * qws_pcap.h - pcapng capture writer for QWS protocol traffic
 *
 * Writes one pcap frame per QWS packet using DLT_USER0 (147).
 * Each frame begins with a 4-byte capture header:
 *
 *   Offset  Size  Field
 *      0      1   direction  (0 = client→server / command,
 *                             1 = server→client / event)
 *      1      1   client_id  (session number, wraps at 255)
 *      2      2   reserved   (zeroed)
 *
 * Followed immediately by the raw QWS wire bytes:
 *
 *      4      4   type       (int32 LE — QWS_CMD_* or QWS_EVT_*)
 *      8      4   raw_len    (int32 LE)
 *     12    var   simpleData
 *   12+s   var   rawData
 *
 * The resulting file can be opened in Wireshark with the
 * wireshark/qws_dissector.lua plugin installed.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_PCAP_H
#define QWS_PCAP_H

#include "qws_proto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qws_pcap_writer qws_pcap_writer_t;

/* Open a new pcapng file for writing.
 * Returns pointer on success, NULL on failure (error printed to stderr). */
qws_pcap_writer_t *qws_pcap_writer_open(const char *path);

/* Capture-frame flags (stored in the reserved field of the capture header).
 * Bit 0: packet was intercepted and not forwarded by the proxy. */
#define QWS_PCAP_FLAG_DROPPED 0x0001u

/* Write one QWS packet as a capture frame.
 * direction: 0 = client→server (command), 1 = server→client (event)
 * client_id: session identifier (truncated to uint8_t)
 * flags: QWS_PCAP_FLAG_DROPPED to mark packets that were not forwarded.
 * Returns 0 on success, -1 on error. */
int qws_pcap_writer_write(qws_pcap_writer_t *w, uint8_t direction,
                          uint8_t client_id, uint16_t flags,
                          const qws_packet_t *pkt);

/* Flush and close the file. NULL-safe. */
void qws_pcap_writer_close(qws_pcap_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif /* QWS_PCAP_H */
