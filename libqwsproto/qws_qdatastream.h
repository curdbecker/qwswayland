/*
 * qws_qdatastream.h - QDataStream serialisation helpers
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_QDATASTREAM_H
#define QWS_QDATASTREAM_H

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write a QDataStream-encoded QString (UTF-16BE, big-endian length prefix).
 * str is treated as UTF-8. Returns 0 on success, -1 on error. */
int qws_fwrite_qdatastream_qstring(FILE *f, const char *str);

/* Write a big-endian int32 (QDataStream int serialisation). */
int qws_fwrite_qdatastream_int32(FILE *f, int32_t val);

/* Write a single byte (QDataStream quint8). */
int qws_fwrite_qdatastream_uint8(FILE *f, uint8_t val);

/* Write a QByteArray: big-endian uint32 byte-count + raw bytes.
 * Pass len < 0 to write a null QByteArray (0xFFFFFFFF). */
int qws_fwrite_qdatastream_bytearray(FILE *f, const void *data, int32_t len);

#ifdef __cplusplus
}
#endif

#endif /* QWS_QDATASTREAM_H */
