/*
 * main.c - QWS trace proxy entry point
 * SPDX-License-Identifier: MIT
 */

#include "trace_proxy.h"
#include "qws_trace.h"
#include "qws_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

static qwstrace_state_t g_state;

static void signal_handler(int sig)
{
    (void)sig;
    /* Force the epoll_wait loop to exit by closing the listen fd.
     * epoll_wait returns EBADF / EINTR and we break out cleanly. */
    if (g_state.epoll_fd >= 0)
        close(g_state.epoll_fd);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "QWS Trace Proxy — man-in-the-middle between a Qt 4.8 QWS client and server.\n"
        "Forwards all traffic verbatim while printing decoded packet traces.\n"
        "\n"
        "Options:\n"
        "  -l, --listen N      QWS display number to listen on (default: 1)\n"
        "  -u, --upstream N    Upstream QWS server display number (default: 0)\n"
        "  -v, --verbose       Increase verbosity (repeatable: -v/-vv/-vvv)\n"
        "                        -v    one-line per packet (type + sizes)\n"
        "                        -vv   decode struct fields\n"
        "                        -vvv  full hex dump of all payloads\n"
        "  -w, --write FILE    Write captured packets to a pcapng file.\n"
        "                        Each frame is one QWS message (DLT_USER0).\n"
        "                        Open with Wireshark + wireshark/qws_dissector.lua.\n"
        "  -x, --exclude LIST  Comma-separated packet type names to suppress.\n"
        "                        Prefix with 'cmd:' or 'evt:' to restrict direction.\n"
        "                        Example: -x mouse,key,cmd:Region\n"
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
        "  qws_trace_proxy -l 1 -u 0 -vv\n"
        "\n"
        "  # Terminal 3: Qt app pointed at display 1\n"
        "  myapp -display ':1'\n"
        "\n",
        prog);
}


int main(int argc, char *argv[])
{
    int listen_display = 1;
    int upstream_display = 0;
    int verbose = 0;
    uint64_t exclude_cmd_mask = 0;
    uint64_t exclude_evt_mask = 0;

    const char *pcap_path = NULL;

    static struct option long_opts[] = {
        { "listen",      required_argument, 0, 'l' },
        { "upstream",    required_argument, 0, 'u' },
        { "verbose",     no_argument,       0, 'v' },
        { "write",       required_argument, 0, 'w' },
        { "exclude",     required_argument, 0, 'x' },
        { "help",        no_argument,       0, 'H' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:u:vw:x:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': listen_display = atoi(optarg); break;
        case 'u': upstream_display = atoi(optarg); break;
        case 'v': verbose++; break;
        case 'w': pcap_path = optarg; break;
        case 'x': qws_trace_parse_exclude_list(optarg, &exclude_cmd_mask, &exclude_evt_mask); break;
        case 'H':
        default:
            usage(argv[0]);
            return (opt == 'H') ? 0 : 1;
        }
    }

    if (verbose > 3) verbose = 3;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    qws_trace_set_level(verbose);
    qws_trace_set_exclude_mask(exclude_cmd_mask, exclude_evt_mask);

    /* Resolve socket paths */
    char listen_path[256];
    if (qws_socket_path(listen_display, listen_path, sizeof(listen_path)) != 0) {
        fprintf(stderr, "Failed to build listen socket path for display %d\n",
                listen_display);
        return 1;
    }

    char upstream_path[256];
    if (qws_socket_path(upstream_display, upstream_path, sizeof(upstream_path)) != 0) {
        fprintf(stderr, "Failed to build upstream socket path for display %d\n",
                upstream_display);
        return 1;
    }

    fprintf(stderr, "qws_trace_proxy: listen=:%d  upstream=:%d  verbose=%d\n",
            listen_display, upstream_display, verbose);

    if (qwstrace_init(&g_state, listen_path, upstream_path) != 0) {
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
    }

    int ret = qwstrace_run(&g_state);
    qwstrace_shutdown(&g_state);
    return ret;
}
