/*
 * qws_unicode.h - UTF-16 / UTF-8 conversion helpers
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_UNICODE_H
#define QWS_UNICODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-16 byte order for conversion functions */
typedef enum {
    QWS_UTF16_LE =
        0, /* little-endian, no BOM (default; matches QWS wire format on x86) */
    QWS_UTF16_BE = 1, /* big-endian, no BOM (QDataStream default byte order) */
} qws_utf16_endian_t;

/* Convert UTF-16LE/BE to UTF-8. src_len is the UTF-16 character count (not bytes).
 * On success: *dst = malloc'd NUL-terminated string (caller frees),
 *             *out_bytes = byte count including NUL (may be NULL), returns 0.
 * On error: *dst = NULL, returns -1. */
int qws_convert_from_utf16(char **dst, const uint8_t *src, size_t src_len,
                           qws_utf16_endian_t endian, size_t *out_bytes);

/* Convert UTF-8 to UTF-16LE/BE. src_len is the UTF-8 character count (not bytes).
 * On success: *dst = malloc'd buffer (caller frees),
 *             *out_bytes = byte count including 2-byte NUL (may be NULL), returns 0.
 * On error: *dst = NULL, returns -1. */
int qws_convert_to_utf16(uint8_t **dst, const char *src, size_t src_len,
                         qws_utf16_endian_t endian, size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* QWS_UNICODE_H */
