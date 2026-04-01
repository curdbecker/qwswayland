/*
 * main.c - QWSWayland proxy daemon entry point
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include "client.h"
#include "lifecycle.h"
#include "qws_pcap.h"
#include "qws_trace.h"

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static qwswl_state_t g_state;

static void signal_handler(int sig) {
    (void)sig;
    /* force shutdown */
    if (g_state.loop_epoll_fd >= 0)
        close(g_state.loop_epoll_fd);
}

static void usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s [options]\n"
        "\n"
        "QWSWayland - QWS to Wayland protocol proxy\n"
        "Allows legacy Qt 4.8/QWS apps to run under a Wayland compositor.\n"
        "\n"
        "Options:\n"
        "  -d, --display N     QWS display number (default: 0)\n"
        "  -w, --width W       Screen width to report to QWS clients (default: "
        "800)\n"
        "  -h, --height H      Screen height to report (default: 480)\n"
        "  -b, --depth D       Color depth in bpp (default: 32)\n"
        "  -v, --verbose LEVEL Trace verbosity level (default: off). LEVEL is "
        "one of:\n"
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
        "  -D, --debug-rects   Draw a red border around each repaint rect\n"
        "  -P, --pcap  FILE    Write captured packets to a pcapng file.\n"
        "                        Each frame is one QWS message (DLT_USER0).\n"
        "                        Open with Wireshark + "
        "wireshark/qws_dissector.lua.\n"
        "  --help              Show this help\n"
        "\n"
        "The proxy creates a QWS server socket at:\n"
        "  /tmp/qtembedded-$USER/QtEmbedded-<display>\n"
        "\n",
        prog);
}

int main(int argc, char *argv[]) {
    const char *pcap_path = NULL;
    qws_pcap_writer_t *pcap_writer = NULL;
    int qws_display = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t depth = 32;
    bool debug_draw_rects = false;

    static struct option long_opts[] = {{"display", required_argument, 0, 'd'},
                                        {"width", required_argument, 0, 'w'},
                                        {"height", required_argument, 0, 'h'},
                                        {"depth", required_argument, 0, 'b'},
                                        {"verbose", required_argument, 0, 'v'},
                                        {"exclude", required_argument, 0, 'x'},
                                        {"include", required_argument, 0, 'i'},
                                        {"debug-rects", no_argument, 0, 'D'},
                                        {"pcap", required_argument, 0, 'P'},
                                        {"help", no_argument, 0, 'H'},
                                        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:b:v:x:i:DP:", long_opts,
                              NULL)) != -1) {
        switch (opt) {
        case 'd':
            qws_display = atoi(optarg);
            break;
        case 'w':
            width = atoi(optarg);
            break;
        case 'h':
            height = atoi(optarg);
            break;
        case 'b':
            depth = atoi(optarg);
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
        case 'D':
            debug_draw_rects = true;
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

    /* Set up signal handling */
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_ign = {.sa_handler = SIG_IGN};
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGPIPE, &sa_ign, NULL); /* Don't die on broken client sockets */

    fprintf(stderr, "QWSWayland v0.1.0 - QWS->Wayland proxy\n");
    fprintf(stderr, "Display :%d, screen %dx%d@%dbpp, level=%d, ipc=%s\n",
            qws_display, width, height, depth, qws_trace_get_level(),
#ifdef QWS_IPC_POSIX
            "posix"
#else
            "sysv"
#endif
    );

    if (pcap_path) {
        pcap_writer = qws_pcap_writer_open(pcap_path);
        if (!pcap_writer) {
            fprintf(stderr, "Failed to open pcap output file: %s\n", pcap_path);
            return 1;
        }
        fprintf(stderr, "qwswayland: writing capture to %s\n", pcap_path);
        qws_trace_set_pcap_writer(pcap_writer);
    }

    if (qwswl_init(&g_state, qws_display, width, height, depth,
                   debug_draw_rects) != 0) {
        fprintf(stderr, "Failed to initialize. Exiting.\n");
        return 1;
    }

    int ret = qwswl_run(&g_state);

    if (g_state.running)
        qwswl_shutdown(&g_state);

    qws_pcap_writer_close(pcap_writer);
    return ret;
}