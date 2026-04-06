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
#include <string.h>
#include <unistd.h>

static qwswl_state_t g_state;

static void signal_handler(int sig) {
    (void)sig;
    /* force shutdown */
    if (g_state.loop_epoll_fd >= 0)
        close(g_state.loop_epoll_fd);
}

static const char USAGE[] =
    "Usage: %s [options]\n"
    "\n"
    "QWSWayland - QWS to Wayland protocol proxy\n"
    "Allows legacy Qt 4.8/QWS apps to run under a Wayland compositor.\n"
    "\n"
    "Options:\n"
    "  -d, --display N              QWS display number (default: 0)\n"
    "  -w, --width W                Screen width to report to QWS clients (default: 800)\n"
    "  -h, --height H               Screen height to report (default: 480)\n"
    "  -b, --depth D                Color depth in bpp (default: 32)\n"
    "  -v, --verbose LEVEL          Trace verbosity level (default: off). LEVEL is one of:\n"
    "                                 off      no tracing\n"
    "                                 basic    one-line per packet (type + sizes)\n"
    "                                 brief    + key fields; bounding box for rects\n"
    "                                 fields   + full decoded struct fields\n"
    "                                 hexdump  + hex dump of all payloads\n"
    "  -x, --exclude LIST           Comma-separated packet type names to suppress.\n"
    "                                 Prefix with 'cmd:' or 'evt:' to restrict direction.\n"
    "                                 Example: -x mouse,key,cmd:Region\n"
    "  -i, --include LIST           Comma-separated packet type names to trace exclusively.\n"
    "                                 When set, only listed types are logged (before -x).\n"
    "                                 Example: -i cmd:RepaintRegion,evt:Region\n"
    "  -D, --debug-rects            Draw a red border around each repaint rect\n"
    "  -s, --screen-driver DRV      Screen driver to use (default: vnc).\n"
    "                                 Valid options: linuxfb, vnc\n"
    "  -f, --fb-device DEV          Framebuffer device path for linuxfb driver\n"
    "                                 (default: /dev/fb0)\n"
    "      --use-interposer         Assume the LD_PRELOAD interposer for the given\n"
    "                                 screen driver has been loaded into QWS clients\n"
    "                                 (default: off)\n"
    "  -P, --pcap FILE              Write captured packets to a pcapng file.\n"
    "                                 Each frame is one QWS message (DLT_USER0).\n"
    "                                 Open with Wireshark + wireshark/qws_dissector.lua.\n"
    "      --use-existing-tempdir   Use the contents of the existing temporary directory without\n"
    "                                 cleaning it up first. This also skips our initialization of\n"
    "                                 the font database and allows a client to use one created\n"
    "                                 by its own Qt QWS server implementation.\n"
    "      --help                   Show this help\n"
    "\n"
    "The proxy creates a QWS server socket at:\n"
    "  /tmp/qtembedded-$USER/QtEmbedded-<display>\n"
    "\n";

int main(int argc, char *argv[]) {
    const char *pcap_path = NULL;
    bool use_existing_tempdir = false;
    qws_pcap_writer_t *pcap_writer = NULL;
    int qws_display = 0;
    bool debug_draw_rects = false;
    /* Raw arguments collected during parsing; struct is built afterwards. */
    qwswl_screen_driver_type_t arg_driver_type = QWSWL_SCREEN_DRIVER_VNC;
    const char *arg_fb_device = "/dev/fb0";
    int32_t arg_width = 0;
    int32_t arg_height = 0;
    int32_t arg_depth = 32;
    bool arg_use_interposer = false;

    static struct option long_opts[] = {
        {"display", required_argument, 0, 'd'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'h'},
        {"depth", required_argument, 0, 'b'},
        {"verbose", required_argument, 0, 'v'},
        {"exclude", required_argument, 0, 'x'},
        {"include", required_argument, 0, 'i'},
        {"debug-rects", no_argument, 0, 'D'},
        {"pcap", required_argument, 0, 'P'},
        {"screen-driver", required_argument, 0, 's'},
        {"fb-device", required_argument, 0, 'f'},
        {"use-interposer", no_argument, 0, 1},
        {"use-existing-tempdir", no_argument, 0, 2},
        {"help", no_argument, 0, 'H'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:b:v:x:i:DP:s:f:", long_opts,
                              NULL)) != -1) {
        switch (opt) {
        case 'd':
            qws_display = atoi(optarg);
            break;
        case 'w':
            arg_width = atoi(optarg);
            break;
        case 'h':
            arg_height = atoi(optarg);
            break;
        case 'b':
            arg_depth = atoi(optarg);
            break;
        case 's':
            if (strcmp(optarg, "linuxfb") != 0 && strcmp(optarg, "vnc") != 0) {
                fprintf(stderr,
                        "Unknown screen driver '%s'. "
                        "Valid options: linuxfb, vnc\n",
                        optarg);
                return 1;
            }
            arg_driver_type = (strcmp(optarg, "linuxfb") == 0)
                                  ? QWSWL_SCREEN_DRIVER_LINUXFB
                                  : QWSWL_SCREEN_DRIVER_VNC;
            break;
        case 'f':
            arg_fb_device = optarg;
            break;
        case 1:
            arg_use_interposer = true;
            break;
        case 2:
            use_existing_tempdir = true;
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
            fprintf(stderr, USAGE, argv[0]);
            return (opt == 'H') ? 0 : 1;
        }
    }

    /* Set up signal handling */
    qwswl_screen_driver_opts_t screen_driver = {
        .type = arg_driver_type,
        .width = arg_width,
        .height = arg_height,
        .depth = arg_depth,
        .use_interposer = arg_use_interposer,
    };
    if (arg_driver_type == QWSWL_SCREEN_DRIVER_LINUXFB)
        snprintf(screen_driver.opts.linuxfb.fb_device,
                 sizeof(screen_driver.opts.linuxfb.fb_device), "%s",
                 arg_fb_device);

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
            qws_display, screen_driver.width, screen_driver.height,
            screen_driver.depth, qws_trace_get_level(),
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

    if (qwswl_init(&g_state, qws_display, debug_draw_rects,
                   use_existing_tempdir, &screen_driver) != 0) {
        fprintf(stderr, "Failed to initialize. Exiting.\n");
        return 1;
    }

    int ret = qwswl_run(&g_state);

    if (g_state.running)
        qwswl_shutdown(&g_state);

    qws_pcap_writer_close(pcap_writer);
    return ret;
}