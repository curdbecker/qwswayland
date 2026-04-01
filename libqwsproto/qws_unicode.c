/*
 * qws_unicode.c - UTF-16 / UTF-8 conversion helpers
 * SPDX-License-Identifier: MIT
 */

#include "qws_unicode.h"
#include <iconv.h>
#include <stdlib.h>

int qws_convert_from_utf16(char **dst, const uint8_t *src, size_t srclen,
                           qws_utf16_endian_t endian, size_t *out_bytes) {
    const char *enc = (endian == QWS_UTF16_BE) ? "UTF-16BE" : "UTF-16LE";
    /* an UTF-8 sequence might need to use several bytes to express the same
     * character, therefore we use the byte size of the UTF-16 string
     * as a simple upper bound. */
    size_t dstlen = srclen * 2;
    char *buf = calloc(dstlen + 1, 1); /* +1: NUL terminator */
    if (!buf) {
        *dst = NULL;
        return -1;
    }

    iconv_t cd = iconv_open("UTF-8", enc);
    if (cd == (iconv_t)-1) {
        free(buf);
        *dst = NULL;
        return -1;
    }

    size_t used;
    {
        /* iconv modifies its pointer arguments; keep in a private scope */
        size_t in_avail = srclen * 2, out_avail = dstlen;
        char *in = (char *)src, *out = buf;
        if (iconv(cd, &in, &in_avail, &out, &out_avail) == (size_t)-1 ||
            in_avail != 0) {
            iconv_close(cd);
            free(buf);
            *dst = NULL;
            return -1;
        }
        used = dstlen - out_avail;
    }
    iconv_close(cd);

    if (out_bytes)
        *out_bytes = used;
    *dst = buf;
    return 0;
}

int qws_convert_to_utf16(uint8_t **dst, const char *src, size_t srclen,
                         qws_utf16_endian_t endian, size_t *out_bytes) {
    const char *enc = (endian == QWS_UTF16_BE) ? "UTF-16BE" : "UTF-16LE";
    size_t dstlen =
        srclen * 2; /* UTF-16: at most 2 bytes per UTF-8 input byte */
    uint8_t *buf = calloc(dstlen + 2, 1); /* +2: UTF-16 NUL sentinel */
    if (!buf) {
        *dst = NULL;
        return -1;
    }

    iconv_t cd = iconv_open(enc, "UTF-8");
    if (cd == (iconv_t)-1) {
        free(buf);
        *dst = NULL;
        return -1;
    }

    size_t used;
    {
        /* iconv modifies its pointer arguments; keep in a private scope */
        size_t in_avail = srclen, out_avail = dstlen;
        char *in = (char *)src, *out = (char *)buf;
        if (iconv(cd, &in, &in_avail, &out, &out_avail) == (size_t)-1 ||
            in_avail != 0) {
            iconv_close(cd);
            free(buf);
            *dst = NULL;
            return -1;
        }
        used = dstlen - out_avail;
    }
    iconv_close(cd);

    if (out_bytes)
        *out_bytes = used;
    *dst = buf;
    return 0;
}
