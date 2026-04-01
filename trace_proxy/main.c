/*
 * main.c - QWS trace proxy entry point
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "qws_proto.h"
#include "qws_trace.h"
#include "trace_proxy.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static qwstrace_state_t g_state;

/* Set to 1 by the signal handler; checked by wait_until_server_ready()
 * in trace_proxy.c to abort during the pre-listen retry loop. */
volatile sig_atomic_t g_interrupted = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
    /* Close listen_fd to unblock the blocking accept() in qwstrace_run(). */
    int fd = g_state.listen_fd;
    if (fd >= 0) {
        g_state.listen_fd = -1;
        close(fd);
    }
}

static void usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s [options]\n"
        "\n"
        "QWS Trace Proxy — man-in-the-middle between a Qt 4.8 QWS client and "
        "server.\n"
        "Forwards all traffic verbatim while printing decoded packet traces.\n"
        "\n"
        "Options:\n"
        "  -l, --listen N      QWS display number to listen on (default: 1)\n"
        "  -u, --upstream N    Upstream QWS server display number (default: "
        "0)\n"
        "  -v, --verbose LEVEL Trace verbosity level (default: basic). LEVEL "
        "is one of:\n"
        "                        off      no tracing\n"
        "                        basic    one-line per packet (type + sizes)\n"
        "                        brief    + key fields; bounding box for "
        "rects\n"
        "                        fields   + full decoded struct fields\n"
        "                        hexdump  + hex dump of all payloads\n"
        "  -x, --exclude LIST  Comma-separated packet type names to suppress.\n"
        "                        Prefix with 'cmd:' or 'evt:' to restrict "
        "direction.\n"
        "                        Example: -x mouse,key,cmd:Region\n"
        "  -i, --include LIST  Comma-separated packet type names to trace "
        "exclusively.\n"
        "                        When set, only listed types are logged "
        "(before -x).\n"
        "                        Example: -i cmd:RepaintRegion,evt:Region\n"
        "  -P, --pcap  FILE    Write captured packets to a pcapng file.\n"
        "                        Each frame is one QWS message (DLT_USER0).\n"
        "                        Open with Wireshark + "
        "wireshark/qws_dissector.lua.\n"
        "  --help              Show this help\n"
        "\n"
        "The proxy listens on:\n"
        "  /tmp/qtembedded-<listen>/QtEmbedded-1\n"
        "and connects upstream to:\n"
        "  /tmp/qtembedded-<server>/QtEmbedded-0\n"
        "\n"
        "Typical usage:\n"
        "  # Terminal 1: real QWS server on display 0\n"
        "  myqws -qws -display ':0'\n"
        "\n"
        "  # Terminal 2: trace proxy\n"
        "  qws_trace_proxy -l 1 -u 0 -v brief\n"
        "\n"
        "  # Terminal 3: Qt app pointed at display 1\n"
        "  myapp -display ':1'\n"
        "\n",
        prog);
}

int main(int argc, char *argv[]) {
    int listen_display = 1;
    int upstream_display = 0;

    const char *pcap_path = NULL;

    /* default to basic so traffic is visible without -v */
    qws_trace_set_level(QWS_TRACE_BASIC);

    static struct option long_opts[] = {{"listen", required_argument, 0, 'l'},
                                        {"upstream", required_argument, 0, 'u'},
                                        {"verbose", required_argument, 0, 'v'},
                                        {"exclude", required_argument, 0, 'x'},
                                        {"include", required_argument, 0, 'i'},
                                        {"pcap", required_argument, 0, 'P'},
                                        {"help", no_argument, 0, 'H'},
                                        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "l:u:v:x:i:P:H", long_opts, NULL)) !=
           -1) {
        switch (opt) {
        case 'l':
            listen_display = atoi(optarg);
            break;
        case 'u':
            upstream_display = atoi(optarg);
            break;
        case 'v':
            if (!qws_trace_parse_level(optarg)) {
                fprintf(stderr,
                        "Unknown trace level '%s'. "
                        "Use: off, basic, brief, fields, hexdump\n",
                        optarg);
                return 1;
            }
            break;
        case 'x':
            if (!qws_trace_parse_exclude_list(optarg)) {
                fprintf(stderr, "Unknown packet type in --exclude list: %s\n",
                        optarg);
                return 1;
            }
            break;
        case 'i':
            if (!qws_trace_parse_include_list(optarg)) {
                fprintf(stderr, "Unknown packet type in --include list: %s\n",
                        optarg);
                return 1;
            }
            break;
        case 'P':
            pcap_path = optarg;
            break;
        case 'H':
        default:
            usage(argv[0]);
            return (opt == 'H') ? 0 : 1;
        }
    }

    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_ign = {.sa_handler = SIG_IGN};
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGPIPE, &sa_ign, NULL);

    fprintf(stderr, "qws_trace_proxy: listen=:%d  upstream=:%d  level=%d\n",
            listen_display, upstream_display, qws_trace_get_level());

    if (qwstrace_init(&g_state, listen_display, upstream_display) != 0) {
        fprintf(stderr, "Initialization failed. Exiting.\n");
        return 1;
    }

    if (pcap_path) {
        g_state.pcap_writer = qws_pcap_writer_open(pcap_path);
        if (!g_state.pcap_writer) {
            fprintf(stderr, "Failed to open pcap output file: %s\n", pcap_path);
            qwstrace_shutdown(&g_state);
            return 1;
        }
        fprintf(stderr, "qws_trace_proxy: writing capture to %s\n", pcap_path);
        qws_trace_set_pcap_writer(g_state.pcap_writer);
    }

    int ret = qwstrace_run(&g_state);
    qwstrace_shutdown(&g_state);
    return ret;
}
