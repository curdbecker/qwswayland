/*
 * qws_pcap.c - pcapng capture writer for QWS protocol traffic
 * SPDX-License-Identifier: MIT
 */

#include "qws_pcap.h"

#include <stdio.h>

#ifdef QWS_HAVE_PCAP

#include <pcap/pcap.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* DLT_USER0 = 147  (libpcap's first user-defined link type) */
#define QWS_PCAP_DLT 147
#define QWS_PCAP_SNAPLEN 65535

/* Capture frame header — precedes the raw QWS wire bytes in each frame */
typedef struct __attribute__((packed)) {
    uint8_t direction; /* 0 = client→server, 1 = server→client */
    uint8_t client_id;
    uint16_t reserved; /* zeroed */
} qws_capture_hdr_t;

struct qws_pcap_writer {
    pcap_t *pcap;
    pcap_dumper_t *dumper;
};

/* ------------------------------------------------------------------ */

qws_pcap_writer_t *qws_pcap_writer_open(const char *path) {
    qws_pcap_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;

    w->pcap = pcap_open_dead(QWS_PCAP_DLT, QWS_PCAP_SNAPLEN);
    if (!w->pcap) {
        fprintf(stderr, "qws_pcap: pcap_open_dead failed\n");
        free(w);
        return NULL;
    }

    w->dumper = pcap_dump_open(w->pcap, path);
    if (!w->dumper) {
        fprintf(stderr, "qws_pcap: cannot open '%s': %s\n", path,
                pcap_geterr(w->pcap));
        pcap_close(w->pcap);
        free(w);
        return NULL;
    }

    return w;
}

/* ------------------------------------------------------------------ */

int qws_pcap_writer_write(qws_pcap_writer_t *w, uint8_t direction,
                          uint8_t client_id, uint16_t flags,
                          const qws_packet_t *pkt) {
    if (!w || !pkt)
        return -1;

    /* Wire bytes for this packet (type + raw_len + simpleData + rawData) */
    size_t wire_len = qws_packet_wire_size(pkt);
    size_t frame_len = sizeof(qws_capture_hdr_t) + wire_len;

    uint8_t *buf = malloc(frame_len);
    if (!buf)
        return -1;

    /* Write capture header */
    qws_capture_hdr_t *cap = (qws_capture_hdr_t *)buf;
    cap->direction = direction;
    cap->client_id = client_id;
    cap->reserved = flags;

    /* Serialize QWS wire bytes immediately after the capture header */
    qws_packet_serialize(pkt, buf + sizeof(qws_capture_hdr_t), wire_len);

    /* Build pcap packet header */
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct pcap_pkthdr hdr;
    hdr.ts.tv_sec = tv.tv_sec;
    hdr.ts.tv_usec = tv.tv_usec;
    hdr.caplen = (bpf_u_int32)frame_len;
    hdr.len = (bpf_u_int32)frame_len;

    pcap_dump((u_char *)w->dumper, &hdr, buf);
    pcap_dump_flush(w->dumper);

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */

void qws_pcap_writer_close(qws_pcap_writer_t *w) {
    if (!w)
        return;
    if (w->dumper)
        pcap_dump_close(w->dumper);
    if (w->pcap)
        pcap_close(w->pcap);
    free(w);
}

#else /* QWS_HAVE_PCAP not available — stub implementations */

/* Opaque stub — never allocated; open() always returns NULL */
struct qws_pcap_writer {
    int _unused;
};

qws_pcap_writer_t *qws_pcap_writer_open(const char *path) {
    (void)path;
    fprintf(stderr, "qws_pcap: built without libpcap, capture unavailable\n");
    return NULL;
}

int qws_pcap_writer_write(qws_pcap_writer_t *w, uint8_t direction,
                          uint8_t client_id, uint16_t flags,
                          const qws_packet_t *pkt) {
    (void)w;
    (void)direction;
    (void)client_id;
    (void)flags;
    (void)pkt;
    return -1;
}

void qws_pcap_writer_close(qws_pcap_writer_t *w) { (void)w; }

#endif /* QWS_HAVE_PCAP */
