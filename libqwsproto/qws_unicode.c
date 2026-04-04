/*
 * qws_unicode.c - UTF-16 / UTF-8 conversion helpers
 * SPDX-License-Identifier: MIT
 */

#include "qws_unicode.h"
#include <iconv.h>
#include <stdlib.h>

/* Allocates a buffer, converts via iconv, and returns it in *out_buf.
 * Returns 0 on success with *used set to bytes written, or -1 on error. */
static int run_iconv(const char *to_enc, const char *from_enc,
                     const char *in_buf, size_t in_bytes, size_t char_bytes,
                     void **out_buf, size_t *out_bytes) {
    *out_buf = NULL;
    if (out_bytes)
        *out_bytes = 0;

    /*
     * UTF-16: at most 2 bytes per UTF-8 input byte
     * UTF-8: UTF-16 max size is a safe output upper bound
     */
    size_t alloc_size = (in_bytes * 2) + char_bytes;
    void *buf = calloc(alloc_size, 1);
    if (!buf)
        return -1;
    iconv_t cd = iconv_open(to_enc, from_enc);
    if (cd == (iconv_t)-1) {
        free(buf);
        return -1;
    }
    /* iconv has the nasty habit of modifying its pointer arguments, so use
     * local variables instead to avoid causing confusing to the caller */
    size_t in_avail = in_bytes, out_avail = alloc_size;
    char *in =
        (char *)in_buf; /* iconv takes char** but does not write to input */
    char *out = buf;
    size_t res = iconv(cd, &in, &in_avail, &out, &out_avail);
    iconv_close(cd);
    if (res == (size_t)-1 || in_avail != 0) {
        free(buf);
        return -1;
    }
    if (out_bytes)
        *out_bytes =
            alloc_size - out_avail + char_bytes; /* include NUL terminator */
    *out_buf = buf;
    return 0;
}

static const char *utf16_enc(qws_utf16_endian_t endian) {
    return (endian == QWS_UTF16_BE) ? "UTF-16BE" : "UTF-16LE";
}

int qws_convert_from_utf16(char **dst, const uint8_t *src, size_t src_len,
                           qws_utf16_endian_t endian, size_t *out_bytes) {
    return run_iconv("UTF-8", utf16_enc(endian), (const char *)src, src_len * 2,
                     1, (void **)dst, out_bytes);
}

int qws_convert_to_utf16(uint8_t **dst, const char *src, size_t src_len,
                         qws_utf16_endian_t endian, size_t *out_bytes) {
    return run_iconv(utf16_enc(endian), "UTF-8", src, src_len, 2, (void **)dst,
                     out_bytes);
}
