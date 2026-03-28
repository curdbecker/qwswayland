/*
 * qws_qdatastream.c - QDataStream serialisation helpers
 * SPDX-License-Identifier: MIT
 */

#include "qws_qdatastream.h"
#include "qws_unicode.h"          /* qws_convert_to_utf16, QWS_UTF16_BE */

#include <stdlib.h>
#include <string.h>

int qws_fwrite_qdatastream_qstring(FILE *f, const char *str)
{
    uint8_t *buf    = NULL;
    size_t   nbytes = 0;
    if (qws_convert_to_utf16(&buf, str, strlen(str),
                                     QWS_UTF16_BE, &nbytes) != 0)
        return -1;

    int ok = qws_fwrite_qdatastream_int32(f, (int32_t)nbytes) == 0
          && fwrite(buf, 1, nbytes, f) == nbytes;
    free(buf);
    return ok ? 0 : -1;
}

int qws_fwrite_qdatastream_int32(FILE *f, int32_t val)
{
    uint32_t u = (uint32_t)val;
    uint8_t be[4] = {
        (uint8_t)(u >> 24), (uint8_t)(u >> 16),
        (uint8_t)(u >>  8), (uint8_t)(u),
    };
    return fwrite(be, 1, 4, f) == 4 ? 0 : -1;
}

int qws_fwrite_qdatastream_uint8(FILE *f, uint8_t val)
{
    return fwrite(&val, 1, 1, f) == 1 ? 0 : -1;
}

int qws_fwrite_qdatastream_bytearray(FILE *f, const void *data, int32_t len)
{
    /* len < 0 → null QByteArray, serialised as -1 (0xFFFFFFFF) */
    if (qws_fwrite_qdatastream_int32(f, len) != 0)
        return -1;
    if (len > 0)
        return fwrite(data, 1, (size_t)len, f) == (size_t)len ? 0 : -1;
    return 0;
}
