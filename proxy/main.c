/*
 * main.c - QWSWayland proxy daemon entry point
 * SPDX-License-Identifier: MIT
 */

#include "lifecycle.h"
#include "client.h"
#include "qws_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

static qwswl_state_t g_state;

static void signal_handler(int sig)
{
    (void)sig;
    /* force shutdown */
    if (g_state.loop_epoll_fd >= 0)
        close(g_state.loop_epoll_fd);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "QWSWayland - QWS to Wayland protocol proxy\n"
        "Allows legacy Qt 4.8/QWS apps to run under a Wayland compositor.\n"
        "\n"
        "Options:\n"
        "  -d, --display N     QWS display number (default: 0)\n"
        "  -w, --width W       Screen width to report to QWS clients (default: 800)\n"
        "  -h, --height H      Screen height to report (default: 480)\n"
        "  -b, --depth D       Color depth in bpp (default: 32)\n"
        "  -v, --verbose       Increase verbosity (can be repeated: -v/-vv/-vvv)\n"
        "                        -v    one-line per packet (type + sizes)\n"
        "                        -vv   decode struct fields\n"
        "                        -vvv  full hex dump of all payloads\n"
        "  -x, --exclude LIST  Comma-separated list of packet type names or numbers to\n"
        "                        suppress from trace output. Prefix with 'cmd:' or 'evt:'\n"
        "                        to restrict to commands or events; unprefixed names are\n"
        "                        matched against both. Example: -x mouse,key,cmd:Region\n"
        "  -D, --debug-rects   Draw a red border around each repaint rect\n"
        "  --help              Show this help\n"
        "\n"
        "The proxy creates a QWS server socket at:\n"
        "  /tmp/qtembedded-$USER/QtEmbedded-<display>\n"
        "\n",
        prog);
}


int main(int argc, char *argv[])
{
    int qws_display = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t depth = 32;
    int verbose = 0;
    bool debug_draw_rects = false;
    uint64_t exclude_cmd_mask = 0;
    uint64_t exclude_evt_mask = 0;

    static struct option long_opts[] = {
        { "display",   required_argument, 0, 'd' },
        { "width",     required_argument, 0, 'w' },
        { "height",    required_argument, 0, 'h' },
        { "depth",     required_argument, 0, 'b' },
        { "verbose",   no_argument,       0, 'v' },
        { "exclude",     required_argument, 0, 'x' },
        { "debug-rects", no_argument,       0, 'D' },
        { "help",        no_argument,       0, 'H' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:b:vx:D", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': qws_display = atoi(optarg); break;
        case 'w': width = atoi(optarg); break;
        case 'h': height = atoi(optarg); break;
        case 'b': depth = atoi(optarg); break;
        case 'v': verbose++; break;
        case 'x': qws_trace_parse_exclude_list(optarg, &exclude_cmd_mask, &exclude_evt_mask); break;
        case 'D': debug_draw_rects = true; break;
        case 'H':
        default:
            usage(argv[0]);
            return (opt == 'H') ? 0 : 1;
        }
    }

    /* Clamp verbose to max level */
    if (verbose > 3) verbose = 3;

    /* Set up signal handling */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  /* Don't die on broken client sockets */

    qws_trace_set_level(verbose);
    qws_trace_set_exclude_mask(exclude_cmd_mask, exclude_evt_mask);

    fprintf(stderr, "QWSWayland v0.1.0 - QWS->Wayland proxy\n");
    fprintf(stderr, "Display :%d, screen %dx%d@%dbpp, verbose=%d, ipc=%s\n",
            qws_display, width, height, depth, verbose,
#ifdef QWS_IPC_POSIX
            "posix"
#else
            "sysv"
#endif
            );

    if (qwswl_init(&g_state, qws_display, width, height, depth,
                    debug_draw_rects) != 0) {
        fprintf(stderr, "Failed to initialize. Exiting.\n");
        return 1;
    }

    int ret = qwswl_run(&g_state);

    if (g_state.running)
        qwswl_shutdown(&g_state);

    return ret;
}