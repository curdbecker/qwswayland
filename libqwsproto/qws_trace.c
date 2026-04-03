/*
 * qws_trace.c - QWS protocol tracing implementation
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "qws_trace.h"
#include "qws_event_factory.h"
#include "qws_rect.h"
#include "qws_unicode.h"
#include <alloca.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

/* ================================================================
 * Global state
 * ================================================================ */

static int g_trace_level = QWS_TRACE_OFF;
static FILE *g_trace_output = NULL;
static uint64_t g_filter_mask = QWS_TRACE_MASK_ALL;
static qws_pcap_writer_t *g_pcap_writer = NULL;

static FILE *trace_fp(void) { return g_trace_output ? g_trace_output : stderr; }

void qws_trace_set_level(int level) { g_trace_level = level; }

int qws_trace_get_level(void) { return g_trace_level; }

void qws_trace_set_output(FILE *fp) { g_trace_output = fp; }

void qws_trace_set_filter_mask(uint64_t mask) { g_filter_mask = mask; }

uint64_t qws_trace_get_filter_mask(void) { return g_filter_mask; }

void qws_trace_set_pcap_writer(qws_pcap_writer_t *w) { g_pcap_writer = w; }

/* Build a combined filter-mask bit pattern from a comma-separated list.
 * CMD types occupy bits 0-31, EVT types bits 32-63. */
static bool parse_filter_list(const char *list, uint64_t *out) {
    char *buf = strdup(list);
    if (!buf)
        return false;

    bool ok = true;
    char *tok = strtok(buf, ",");
    while (tok) {
        int check_cmd = 1, check_evt = 1;
        const char *name = tok;

        if (strncmp(tok, "cmd:", 4) == 0) {
            name = tok + 4;
            check_evt = 0;
        } else if (strncmp(tok, "evt:", 4) == 0) {
            name = tok + 4;
            check_cmd = 0;
        }

        int matched = 0;
        for (int i = 0; i < 32; i++) {
            if (check_cmd && strcasecmp(qws_command_type_name(i), name) == 0) {
                *out |= QWS_TRACE_CMD_BIT(i);
                matched = 1;
            }
            if (check_evt && strcasecmp(qws_event_type_name(i), name) == 0) {
                *out |= QWS_TRACE_EVT_BIT(i);
                matched = 1;
            }
        }
        if (!matched) {
            fprintf(stderr, "Warning: unknown packet type name '%s'\n", tok);
            fprintf(stderr, "Available commands:");
            for (int i = 0; i < 32; i++) {
                const char *n = qws_command_type_name(i);
                if (strcmp(n, "Unknown") != 0)
                    fprintf(stderr, " %s", n);
            }
            fprintf(stderr, "\nAvailable events:");
            for (int i = 0; i < QWS_EVT_NEVENT; i++) {
                const char *n = qws_event_type_name(i);
                if (strcmp(n, "Unknown") != 0)
                    fprintf(stderr, " %s", n);
            }
            fprintf(stderr, "\n");
            ok = false;
            break;
        }

        tok = strtok(NULL, ",");
    }

    free(buf);
    return ok;
}

bool qws_trace_parse_exclude_list(const char *list) {
    uint64_t bits = 0;
    if (!parse_filter_list(list, &bits))
        return false;
    g_filter_mask &= ~bits;
    return true;
}

bool qws_trace_parse_include_list(const char *list) {
    uint64_t bits = 0;
    if (!parse_filter_list(list, &bits))
        return false;
    g_filter_mask = bits;
    return true;
}

bool qws_trace_parse_level(const char *name) {
    static const struct {
        const char *name;
        int level;
    } levels[] = {
        {"off", QWS_TRACE_OFF},         {"basic", QWS_TRACE_BASIC},
        {"brief", QWS_TRACE_BRIEF},     {"fields", QWS_TRACE_FIELDS},
        {"hexdump", QWS_TRACE_HEXDUMP},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcasecmp(name, levels[i].name) == 0) {
            qws_trace_set_level(levels[i].level);
            return true;
        }
    }
    /* Accept bare integers */
    char *end;
    long v = strtol(name, &end, 10);
    if (*end == '\0' && v >= QWS_TRACE_OFF && v <= QWS_TRACE_HEXDUMP) {
        qws_trace_set_level((int)v);
        return true;
    }
    return false;
}

/* ================================================================
 * Timestamp helper
 * ================================================================ */

static void print_timestamp(FILE *fp) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    fprintf(fp, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            (long)(tv.tv_usec / 1000));
}

/* ================================================================
 * Hex dump
 * ================================================================ */

void qws_trace_hexdump(FILE *fp, const char *indent, const void *data,
                       size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    const char *pfx = indent ? indent : "  ";

    for (size_t off = 0; off < len; off += 16) {
        fprintf(fp, "%s%04zx: ", pfx, off);

        /* Hex bytes */
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len)
                fprintf(fp, "%02x ", p[off + i]);
            else
                fprintf(fp, "   ");
            if (i == 7)
                fprintf(fp, " ");
        }

        /* ASCII */
        fprintf(fp, " |");
        for (size_t i = 0; i < 16 && (off + i) < len; i++) {
            uint8_t c = p[off + i];
            fprintf(fp, "%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        fprintf(fp, "|\n");
    }
}

/* ================================================================
 * Qt enum value decoders (for readable output)
 * ================================================================ */

static const char *qt_mouse_button_str(int32_t state) {
    static char buf[128];
    buf[0] = '\0';

    if (state == 0)
        return "NoButton";
    if (state & 0x01)
        strcat(buf, "Left");
    if (state & 0x02) {
        if (buf[0])
            strcat(buf, "|");
        strcat(buf, "Right");
    }
    if (state & 0x04) {
        if (buf[0])
            strcat(buf, "|");
        strcat(buf, "Middle");
    }
    if (state & 0x08) {
        if (buf[0])
            strcat(buf, "|");
        strcat(buf, "XButton1");
    }
    if (state & 0x10) {
        if (buf[0])
            strcat(buf, "|");
        strcat(buf, "XButton2");
    }

    /* Keyboard modifiers in upper bits */
    if (state & 0x02000000) {
        if (buf[0])
            strcat(buf, "+");
        strcat(buf, "Shift");
    }
    if (state & 0x04000000) {
        if (buf[0])
            strcat(buf, "+");
        strcat(buf, "Ctrl");
    }
    if (state & 0x08000000) {
        if (buf[0])
            strcat(buf, "+");
        strcat(buf, "Alt");
    }
    if (state & 0x10000000) {
        if (buf[0])
            strcat(buf, "+");
        strcat(buf, "Meta");
    }

    return buf[0] ? buf : "0";
}

static void print_surface_flags(FILE *fp, int32_t flags) {
    if (flags == 0) {
        fprintf(fp, "0");
        return;
    }
    bool first = true;
    if (flags & QWS_SURFACE_REGION_RESERVED) {
        fprintf(fp, "RegionReserved");
        first = false;
    }
    if (flags & QWS_SURFACE_BUFFERED) {
        fprintf(fp, "%sBuffered", first ? "" : "|");
        first = false;
    }
    if (flags & QWS_SURFACE_OPAQUE) {
        fprintf(fp, "%sOpaque", first ? "" : "|");
    }
}

void qws_trace_print_rects(FILE *fp, const qws_rect_t *rects, int nr_rects) {
    for (int i = 0; i < nr_rects; i++) {
        fprintf(fp, "        rect[%d]: (%d,%d) (%d,%d)\n", i, rects[i].x1,
                rects[i].y1, rects[i].x2, rects[i].y2);
    }
}

void qws_trace_print_rects_bbox(FILE *fp, const qws_rect_t *rects,
                                int nr_rects) {
    int32_t x1, y1, x2, y2;
    qws_rect_bounding_box(rects, nr_rects, &x1, &y1, &x2, &y2);
    fprintf(fp, "        bbox: (%d,%d) (%d,%d) [%d rects]\n", x1, y1, x2, y2,
            nr_rects);
}

static void print_window_flags(FILE *fp, uint32_t flags) {
    fprintf(fp, "  window_flags: 0x%08x  type=%s%s\n", flags,
            qws_window_type_str(flags),
            QWS_IS_TOPLEVEL_TYPE(flags) ? " [toplevel]" : "");

    static const struct {
        uint32_t bit;
        const char *name;
    } hints[] = {
        {QWS_WF_MSWINDOWS_FIXED_SIZE, "MSWindowsFixedSizeDialog"},
        {QWS_WF_MSWINDOWS_OWN_DC, "MSWindowsOwnDC"},
        {QWS_WF_X11_BYPASS_WM, "X11BypassWindowManager"},
        {QWS_WF_FRAMELESS, "Frameless"},
        {QWS_WF_TITLE, "WindowTitle"},
        {QWS_WF_SYSTEM_MENU, "WindowSystemMenu"},
        {QWS_WF_MINIMIZE_BUTTON, "WindowMinimizeButton"},
        {QWS_WF_MAXIMIZE_BUTTON, "WindowMaximizeButton"},
        {QWS_WF_CONTEXT_HELP_BUTTON, "WindowContextHelpButton"},
        {QWS_WF_SHADE_BUTTON, "WindowShadeButton"},
        {QWS_WF_STAYS_ON_TOP, "WindowStaysOnTop"},
        {QWS_WF_OK_BUTTON, "WindowOkButton"},
        {QWS_WF_CANCEL_BUTTON, "WindowCancelButton"},
        {QWS_WF_CUSTOMIZE, "CustomizeWindow"},
        {QWS_WF_STAYS_ON_BOTTOM, "WindowStaysOnBottom"},
        {QWS_WF_CLOSE_BUTTON, "WindowCloseButton"},
        {QWS_WF_MAC_TOOLBAR_BUTTON, "MacWindowToolBarButton"},
        {QWS_WF_BYPASS_GRAPHICS_PROXY, "BypassGraphicsProxy"},
        {QWS_WF_SOFTKEYS_VISIBLE, "WindowSoftkeysVisible"},
        {QWS_WF_SOFTKEYS_RESPOND, "WindowSoftkeysRespond"},
    };

    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (flags & hints[i].bit)
            fprintf(fp, "    + %s\n", hints[i].name);
    }
}

static void print_utf16le_field(FILE *fp, const char *field_name,
                                const uint8_t *field_value, size_t field_len) {
    char *value;

    if (!field_value || field_len <= 0) {
        fprintf(fp, "      %s=<not present>\n", field_name);
        return;
    }

    if (qws_convert_from_utf16(&value, field_value, field_len, QWS_UTF16_LE,
                               NULL) != 0) {
        fprintf(fp, "      %s=<invalid>\n", field_name);
    } else {
        fprintf(fp, "      %s=\"%s\"\n", field_name, value);
        free(value);
    }
}

/* ================================================================
 * Event field decoder
 * ================================================================ */

void qws_trace_decode_event(FILE *fp, int32_t type, const void *simple_data,
                            int32_t simple_len, const void *raw_data,
                            int32_t raw_len, bool brief) {

    switch (type) {

    case QWS_EVT_CONNECTED: {
        if (simple_len >= (int32_t)sizeof(qws_evt_connected_t)) {
            const qws_evt_connected_t *d = simple_data;
            fprintf(
                fp,
                "      client_id=%d, server_shm_id=%d, display_spec_len=%d\n",
                d->client_id, d->server_shm_id, d->len);
            if (raw_data && d->len > 0) {
                int plen = d->len;
                if (plen > 80)
                    plen = 80;
                fprintf(fp, "        display_spec=\"%.*s\"\n", plen,
                        (const char *)raw_data);
            }
        }
        break;
    }

    case QWS_EVT_MOUSE: {
        if (simple_len >= (int32_t)sizeof(qws_evt_mouse_t)) {
            const qws_evt_mouse_t *d = simple_data;
            fprintf(fp,
                    "      window=%d, pos=(%d,%d), state=%s(0x%x), time=%dms\n",
                    d->window, d->x_root, d->y_root,
                    qt_mouse_button_str(d->state), d->state, d->time);
        }
        break;
    }

    case QWS_EVT_KEY: {
        if (simple_len >= (int32_t)sizeof(qws_evt_key_t)) {
            const qws_evt_key_t *d = simple_data;
            fprintf(fp,
                    "      window=%d, unicode=0x%x('%c'), keycode=0x%x, "
                    "mod=0x%x, %s%s\n",
                    d->window, d->unicode,
                    (d->unicode >= 0x20 && d->unicode < 0x7f) ? (char)d->unicode
                                                              : '?',
                    d->keycode, d->modifiers,
                    (d->flags & QWS_KEY_FLAG_PRESS) ? "PRESS" : "RELEASE",
                    (d->flags & QWS_KEY_FLAG_AUTO_REPEAT) ? " (repeat)" : "");
        }
        break;
    }

    case QWS_EVT_FOCUS: {
        if (simple_len >= (int32_t)sizeof(qws_evt_focus_t)) {
            const qws_evt_focus_t *d = simple_data;
            fprintf(fp, "      window=%d, %s\n", d->window,
                    d->get_focus ? "FOCUS_IN" : "FOCUS_OUT");
        }
        break;
    }

    case QWS_EVT_REGION: {
        if (simple_len >= (int32_t)sizeof(qws_evt_region_t)) {
            const qws_evt_region_t *d = simple_data;
            fprintf(fp, "      window=%d, nrects=%d, type=%d\n", d->window,
                    d->nrectangles, d->type);
            /* Decode rectangles from raw data */
            if (raw_data && raw_len > 0) {
                if (brief && d->nrectangles > 1)
                    qws_trace_print_rects_bbox(fp, (qws_rect_t *)raw_data,
                                               d->nrectangles);
                else
                    qws_trace_print_rects(fp, (qws_rect_t *)raw_data,
                                          d->nrectangles);
            }
        }
        break;
    }

    case QWS_EVT_CREATION: {
        if (simple_len >= (int32_t)sizeof(qws_evt_creation_t)) {
            const qws_evt_creation_t *d = simple_data;
            fprintf(fp, "      object_id=%d, count=%d (range %d..%d)\n",
                    d->object_id, d->count, d->object_id,
                    d->object_id + d->count - 1);
        }
        break;
    }

    case QWS_EVT_MAX_WINDOW_RECT: {
        if (simple_len >= (int32_t)sizeof(qws_evt_max_window_rect_t)) {
            const qws_evt_max_window_rect_t *d = simple_data;
            fprintf(fp, "      window=%d\n", d->window);
            qws_trace_print_rects(fp, &d->rect, 1);
        }
        break;
    }

    case QWS_EVT_PROPERTY_NOTIFY: {
        if (simple_len >= (int32_t)sizeof(qws_evt_property_notify_t)) {
            const qws_evt_property_notify_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d, state=%s\n", d->window,
                    d->property, d->state == 0 ? "Changed" : "Deleted");
        }
        break;
    }

    case QWS_EVT_PROPERTY_REPLY: {
        if (simple_len >= (int32_t)sizeof(qws_evt_property_reply_t)) {
            const qws_evt_property_reply_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d, value_len=%d\n",
                    d->window, d->property, d->len);
        }
        break;
    }

    case QWS_EVT_WINDOW_OPERATION: {
        if (simple_len >= (int32_t)sizeof(qws_evt_window_operation_t)) {
            const qws_evt_window_operation_t *d = simple_data;
            fprintf(fp, "      window=%d, operation=%d\n", d->window,
                    d->operation);
        }
        break;
    }

    case QWS_EVT_EMBED: {
        if (simple_len >= (int32_t)sizeof(qws_evt_embed_t)) {
            const qws_evt_embed_t *d = simple_data;
            fprintf(fp, "      window=%d, type=%d\n", d->window, d->type);
        }
        break;
    }

    case QWS_EVT_SELECTION_CLEAR: {
        if (simple_len >= (int32_t)sizeof(qws_evt_selection_clear_t)) {
            const qws_evt_selection_clear_t *d = simple_data;
            fprintf(fp, "      window=%d\n", d->window);
        }
        break;
    }

    case QWS_EVT_SELECTION_REQUEST: {
        if (simple_len >= (int32_t)sizeof(qws_evt_selection_request_t)) {
            const qws_evt_selection_request_t *d = simple_data;
            fprintf(
                fp,
                "      window=%d, requestor=%d, property=%d, mimetypes=%d\n",
                d->window, d->requestor, d->property, d->mimetypes);
        }
        break;
    }

    case QWS_EVT_SELECTION_NOTIFY: {
        if (simple_len >= (int32_t)sizeof(qws_evt_selection_notify_t)) {
            const qws_evt_selection_notify_t *d = simple_data;
            fprintf(fp,
                    "      window=%d, requestor=%d, property=%d, mimetype=%d\n",
                    d->window, d->requestor, d->property, d->mimetype);
        }
        break;
    }

    case QWS_EVT_QCOP_MESSAGE: {
        if (simple_len >= (int32_t)sizeof(qws_evt_qcop_message_t)) {
            const qws_evt_qcop_message_t *d = simple_data;
            fprintf(
                fp,
                "      is_response=%d, lchannel=%d, lmessage=%d, ldata=%d\n",
                d->is_response, d->lchannel, d->lmessage, d->ldata);
            if (raw_data && raw_len > 0) {
                size_t ch_bytes = (size_t)d->lchannel * 2;
                print_utf16le_field(fp, "channel", raw_data,
                                    (size_t)d->lchannel);
                if ((size_t)raw_len >= ch_bytes)
                    print_utf16le_field(fp, "message",
                                        &((const uint8_t *)raw_data)[ch_bytes],
                                        (size_t)d->lmessage);
            }
        }
        break;
    }

    case QWS_EVT_IM_EVENT: {
        if (simple_len >= (int32_t)sizeof(qws_evt_im_event_t)) {
            const qws_evt_im_event_t *d = simple_data;
            fprintf(fp,
                    "      window=%d, replace_from=%d, replace_length=%d, "
                    "data_len=%d\n",
                    d->window, d->replace_from, d->replace_length, raw_len);
        }
        break;
    }

    case QWS_EVT_IM_QUERY: {
        if (simple_len >= (int32_t)sizeof(qws_evt_im_query_t)) {
            const qws_evt_im_query_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d\n", d->window,
                    d->property);
        }
        break;
    }

    case QWS_EVT_IM_INIT: {
        if (simple_len >= (int32_t)sizeof(qws_evt_im_init_t)) {
            const qws_evt_im_init_t *d = simple_data;
            fprintf(fp, "      window=%d, existence=%d (%s)\n", d->window,
                    d->existence, d->existence ? "created" : "destroyed");
        }
        break;
    }

    case QWS_EVT_FONT: {
        if (simple_len >= (int32_t)sizeof(qws_evt_font_t)) {
            const qws_evt_font_t *d = simple_data;
            fprintf(fp, "      type=%s\n",
                    d->type == FONT_REMOVED ? "FontRemoved" : "unknown");
        }
        break;
    }

    case QWS_EVT_SCREEN_TRANSFORM: {
        if (simple_len >= (int32_t)sizeof(qws_evt_screen_transform_t)) {
            const qws_evt_screen_transform_t *d = simple_data;
            fprintf(fp, "      screen=%d, transformation=%d\n", d->screen,
                    d->transformation);
        }
        break;
    }

    default:
        if (simple_len > 0 && simple_data) {
            /* Generic: show first int which is usually window id */
            const int32_t *p = simple_data;
            fprintf(fp, "      [field0=%d (0x%x)]\n", p[0], p[0]);
        }
        break;
    }
}

/* ================================================================
 * Command field decoder
 * ================================================================ */

void qws_trace_decode_command(FILE *fp, int32_t type, const void *simple_data,
                              int32_t simple_len, const void *raw_data,
                              int32_t raw_len, bool brief) {
    (void)raw_data;

    switch (type) {

    case QWS_CMD_IDENTIFY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_identify_t)) {
            const qws_cmd_identify_t *d = simple_data;
            fprintf(fp, "      id_lock=%d, id_len=%d\n", d->id_lock, d->id_len);
            print_utf16le_field(fp, "app_name", raw_data, d->id_len * 2);
        }
        break;
    }

    case QWS_CMD_CREATE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_create_t)) {
            const qws_cmd_create_t *d = simple_data;
            fprintf(fp, "      count=%d\n", d->count);
        }
        break;
    }

    case QWS_CMD_REGION: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_region_request_t)) {
            const qws_cmd_region_request_t *d = simple_data;
            fprintf(fp,
                    "      window=%d, surfacekeylength=%d, "
                    "surfacedatalength=%d, nrects=%d\n",
                    d->window, d->surfacekeylength, d->surfacedatalength,
                    d->nrectangles);
            if (raw_data && raw_len > 0) {
                char *surface_key;
                const qws_cmd_region_surface_data_t *surface_data =
                    (qws_cmd_region_surface_data_t *)&(
                        (char *)raw_data)[d->nrectangles * sizeof(qws_rect_t) +
                                          d->surfacekeylength * 2];

                if (brief && d->nrectangles > 1)
                    qws_trace_print_rects_bbox(fp, (qws_rect_t *)raw_data,
                                               d->nrectangles);
                else
                    qws_trace_print_rects(fp, (qws_rect_t *)raw_data,
                                          d->nrectangles);

                if (qws_convert_from_utf16(
                        &surface_key,
                        (const uint8_t *)&(
                            (char *)
                                raw_data)[d->nrectangles * sizeof(qws_rect_t)],
                        d->surfacekeylength, QWS_UTF16_LE, NULL) != 0) {
                    fprintf(fp, "       surfacekey UTF-16 invalid sequence!\n");
                    fprintf(fp, "       raw_data=\n");
                    qws_trace_hexdump(fp, "            ", raw_data, raw_len);
                    break;
                }

                fprintf(fp, "      surfacekey=%s\n", surface_key);
                fprintf(fp, "      surfacedata=\n");
                if (!strcmp(surface_key, "shm")) {
                    fprintf(fp,
                            "        mem_id=%d, width=%d, height=%d, "
                            "lock_id=%d, format=%s\n",
                            surface_data->shm.mem_id, surface_data->shm.width,
                            surface_data->shm.height, surface_data->shm.lock_id,
                            qws_image_format_name(surface_data->shm.format));
                    fprintf(fp, "        flags=");
                    print_surface_flags(fp, surface_data->shm.flags);
                    fprintf(fp, "\n");
                } else {
                    qws_trace_hexdump(fp, "            ", surface_data->raw,
                                      d->surfacedatalength);
                }

                free(surface_key);
            }
        }
        break;
    }

    case QWS_CMD_REPAINT_REGION: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_repaint_region_t)) {
            const qws_cmd_repaint_region_t *d = simple_data;
            fprintf(fp, "      window=%d, nrects=%d, opaque=%d\n", d->window,
                    d->nrectangles, d->opaque);
            if (!brief)
                print_window_flags(fp, d->window_flags);
            if (raw_data && raw_len > 0) {
                if (brief && d->nrectangles > 1)
                    qws_trace_print_rects_bbox(fp, (qws_rect_t *)raw_data,
                                               d->nrectangles);
                else
                    qws_trace_print_rects(fp, (qws_rect_t *)raw_data,
                                          d->nrectangles);
            }
        }
        break;
    }

    case QWS_CMD_REGION_MOVE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_region_move_t)) {
            const qws_cmd_region_move_t *d = simple_data;
            fprintf(fp, "      window=%d, delta=(%d,%d)\n", d->window, d->dx,
                    d->dy);
        }
        break;
    }

    case QWS_CMD_REGION_DESTROY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_region_destroy_t)) {
            const qws_cmd_region_destroy_t *d = simple_data;
            fprintf(fp, "      window=%d\n", d->window);
        }
        break;
    }

    case QWS_CMD_REGION_NAME: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_region_name_t)) {
            const qws_cmd_region_name_t *d = simple_data;
            fprintf(fp, "      window=%d, name_bytes=%d, caption_bytes=%d\n",
                    d->window, d->name_bytes, d->caption_bytes);
            print_utf16le_field(fp, "name", raw_data, d->name_bytes / 2);
            print_utf16le_field(fp, "caption",
                                &((uint8_t *)raw_data)[d->name_bytes],
                                d->caption_bytes / 2);
        }
        break;
    }

    case QWS_CMD_CHANGE_ALTITUDE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_change_altitude_t)) {
            const qws_cmd_change_altitude_t *d = simple_data;
            fprintf(fp, "      window=%d, altitude=%s(%d), fixed=%d\n",
                    d->window, qws_altitude_name(d->altitude), d->altitude,
                    d->is_fixed);
        }
        break;
    }

    case QWS_CMD_REQUEST_FOCUS: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_request_focus_t)) {
            const qws_cmd_request_focus_t *d = simple_data;
            fprintf(fp, "      window=%d %s\n", d->window,
                    qws_focus_flag_str(d->flag));
        }
        break;
    }

    case QWS_CMD_SET_OPACITY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_set_opacity_t)) {
            const qws_cmd_set_opacity_t *d = simple_data;
            fprintf(fp, "      window=%d, opacity=%d (%.0f%%)\n", d->window,
                    d->opacity, (double)d->opacity * 100.0 / 255.0);
        }
        break;
    }

    case QWS_CMD_ADD_PROPERTY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_add_property_t)) {
            const qws_cmd_add_property_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d\n", d->window,
                    d->property);
        }
        break;
    }

    case QWS_CMD_SET_PROPERTY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_set_property_t)) {
            const qws_cmd_set_property_t *d = simple_data;
            const char *mode_str = "Replace";
            if (d->mode == 1)
                mode_str = "Append";
            if (d->mode == 2)
                mode_str = "Prepend";
            fprintf(fp,
                    "      window=%d, property=%d, mode=%s, "
                    "value_len=%d\n",
                    d->window, d->property, mode_str, raw_len);
        }
        break;
    }

    case QWS_CMD_REMOVE_PROPERTY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_remove_property_t)) {
            const qws_cmd_remove_property_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d\n", d->window,
                    d->property);
        }
        break;
    }

    case QWS_CMD_GET_PROPERTY: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_get_property_t)) {
            const qws_cmd_get_property_t *d = simple_data;
            fprintf(fp, "      window=%d, property=%d\n", d->window,
                    d->property);
        }
        break;
    }

    case QWS_CMD_GRAB_MOUSE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_grab_mouse_t)) {
            const qws_cmd_grab_mouse_t *d = simple_data;
            fprintf(fp, "      window=%d, %s\n", d->window,
                    d->grab ? "GRAB" : "UNGRAB");
        }
        break;
    }

    case QWS_CMD_GRAB_KEYBOARD: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_grab_keyboard_t)) {
            const qws_cmd_grab_keyboard_t *d = simple_data;
            fprintf(fp, "      window=%d, %s\n", d->window,
                    d->grab ? "GRAB" : "UNGRAB");
        }
        break;
    }

    case QWS_CMD_DEFINE_CURSOR: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_define_cursor_t)) {
            const qws_cmd_define_cursor_t *d = simple_data;
            fprintf(fp,
                    "      size=%dx%d, hot=(%d,%d), id=%d, "
                    "data_len=%d\n",
                    d->width, d->height, d->hot_x, d->hot_y, d->id, raw_len);
        }
        break;
    }

    case QWS_CMD_SELECT_CURSOR: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_select_cursor_t)) {
            const qws_cmd_select_cursor_t *d = simple_data;
            fprintf(fp, "      window=%d, cursor_id=%d\n", d->window,
                    d->cursor_id);
        }
        break;
    }

    case QWS_CMD_QCOP_REGISTER: {
        fprintf(fp, "      channel (in rawData, %d bytes)\n", raw_len);
        break;
    }

    case QWS_CMD_QCOP_SEND: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_qcop_send_t)) {
            const qws_cmd_qcop_send_t *d = simple_data;
            fprintf(fp, "      channel_len=%d, message_len=%d, data_len=%d\n",
                    d->channel_len, d->message_len, d->data_len);
        }
        break;
    }

    case QWS_CMD_FONT: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_font_t)) {
            const qws_cmd_font_t *d = simple_data;
            fprintf(fp, "      type=%s\n",
                    d->type == 0 ? "StartedUsing" : "StoppedUsing");
            if (raw_data && raw_len > 0)
                fprintf(fp, "      font_name=\"%.*s\"\n", raw_len,
                        (const char *)raw_data);
        }
        break;
    }

    case QWS_CMD_IM_MOUSE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_im_mouse_t)) {
            const qws_cmd_im_mouse_t *d = simple_data;
            fprintf(fp, "      window=%d, index=%d, state=%d\n", d->window,
                    d->index, d->state);
        }
        break;
    }

    case QWS_CMD_IM_UPDATE: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_im_update_t)) {
            const qws_cmd_im_update_t *d = simple_data;
            fprintf(fp, "      window=%d, type=%s(%d), widget_id=%d\n",
                    d->window, qws_im_update_type_name(d->type), d->type,
                    d->widget_id);
        }
        break;
    }

    case QWS_CMD_SHUTDOWN: {
        fprintf(fp, "      (no payload)\n");
        break;
    }

    case QWS_CMD_SET_SELECTION_OWNER: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_set_selection_owner_t)) {
            const qws_cmd_set_selection_owner_t *d = simple_data;
            fprintf(fp, "      window=%d, time=%02d:%02d:%02d.%03d\n",
                    d->windowid, d->hour, d->minute, d->sec, d->ms);
        }
        break;
    }

    case QWS_CMD_CONVERT_SELECTION: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_convert_selection_t)) {
            const qws_cmd_convert_selection_t *d = simple_data;
            fprintf(fp, "      requestor=%d, selection=%d, mimetypes=%d\n",
                    d->requestor, d->selection, d->mimetypes);
        }
        break;
    }

    case QWS_CMD_POSITION_CURSOR: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_position_cursor_t)) {
            const qws_cmd_position_cursor_t *d = simple_data;
            fprintf(fp, "      pos=(%d,%d)\n", d->new_x, d->new_y);
        }
        break;
    }

    case QWS_CMD_PLAY_SOUND: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_play_sound_t)) {
            const qws_cmd_play_sound_t *d = simple_data;
            fprintf(fp, "      window=%d\n", d->windowid);
            print_utf16le_field(fp, "filename", raw_data,
                                (size_t)(raw_len / 2));
        }
        break;
    }

    case QWS_CMD_EMBED: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_embed_t)) {
            const qws_cmd_embed_t *d = simple_data;
            const char *type_str = (d->type == QWS_EMBED_START)    ? "Start"
                                   : (d->type == QWS_EMBED_STOP)   ? "Stop"
                                   : (d->type == QWS_EMBED_REGION) ? "Region"
                                                                   : "unknown";
            fprintf(fp,
                    "      embedder=%d, embedded=%d, type=%s(%d), nrects=%d\n",
                    d->embedder, d->embedded, type_str, d->type, d->nrects);
            if (raw_data && d->nrects > 0) {
                if (brief && d->nrects > 1)
                    qws_trace_print_rects_bbox(fp, (const qws_rect_t *)raw_data,
                                               d->nrects);
                else
                    qws_trace_print_rects(fp, (const qws_rect_t *)raw_data,
                                          d->nrects);
            }
        }
        break;
    }

    case QWS_CMD_SCREEN_TRANSFORM: {
        if (simple_len >= (int32_t)sizeof(qws_cmd_screen_transform_t)) {
            const qws_cmd_screen_transform_t *d = simple_data;
            fprintf(fp, "      screen=%d, transformation=%d\n", d->screen,
                    d->transformation);
        }
        break;
    }

    default:
        if (simple_len > 0 && simple_data) {
            const int32_t *p = simple_data;
            int nfields = simple_len / 4;
            if (nfields > 8)
                nfields = 8;
            fprintf(fp, "      fields[%d]:", nfields);
            for (int i = 0; i < nfields; i++)
                fprintf(fp, " %d", p[i]);
            if (simple_len / 4 > 8)
                fprintf(fp, " ...");
            fprintf(fp, "\n");
        }
        break;
    }
}

/* ================================================================
 * Main packet trace function
 * ================================================================ */

void qws_trace_packet(const int32_t client_id, const qws_packet_t *pkt,
                      bool outgoing) {
    if (!pkt)
        return;

    /* PCAP capture: independent of trace level and exclusion masks */
    if (g_pcap_writer)
        qws_pcap_writer_write(g_pcap_writer, outgoing ? 1 : 0,
                              (uint8_t)client_id, pkt);

    if (g_trace_level <= QWS_TRACE_OFF)
        return;

    FILE *fp = trace_fp();
    int32_t type = pkt->header.type;

    /* Check filter mask before doing any work */
    if (type >= 0 && type < 32) {
        uint64_t bit =
            outgoing ? QWS_TRACE_EVT_BIT(type) : QWS_TRACE_CMD_BIT(type);
        if (!(g_filter_mask & bit))
            return;
    }

    const char *type_name =
        outgoing ? qws_event_type_name(type) : qws_command_type_name(type);

    /* Level 1+: one-line summary */
    print_timestamp(fp);
    fprintf(fp, "%s ", outgoing ? "<<<" : ">>>");
    if (client_id > 0)
        fprintf(fp, "[client %d] ", client_id);
    fprintf(fp, "%s %s (type=0x%x, simple=%d, raw=%d)\n",
            outgoing ? "EVT" : "CMD", type_name, type, pkt->header.simple_len,
            pkt->header.raw_len);

    /* Level 2+: decoded fields (brief at 2, full at 3+) */
    if (g_trace_level >= QWS_TRACE_BRIEF) {
        bool brief = (g_trace_level < QWS_TRACE_FIELDS);
        if (outgoing) {
            qws_trace_decode_event(fp, type, pkt->simple_data,
                                   pkt->header.simple_len, pkt->raw_data,
                                   pkt->header.raw_len, brief);
        } else {
            qws_trace_decode_command(fp, type, pkt->simple_data,
                                     pkt->header.simple_len, pkt->raw_data,
                                     pkt->header.raw_len, brief);
        }
    }

    /* Level 4+: hex dump of all payload data */
    if (g_trace_level >= QWS_TRACE_HEXDUMP) {
        if (pkt->header.simple_len > 0 && pkt->simple_data) {
            fprintf(fp, "    simpleData (%d bytes):\n", pkt->header.simple_len);
            qws_trace_hexdump(fp, "      ", pkt->simple_data,
                              (size_t)pkt->header.simple_len);
        }
        if (pkt->header.raw_len > 0 && pkt->raw_data) {
            fprintf(fp, "    rawData (%d bytes):\n", pkt->header.raw_len);
            qws_trace_hexdump(fp, "      ", pkt->raw_data,
                              (size_t)pkt->header.raw_len);
        }
    }

    fflush(fp);
}

/* ================================================================
 * Raw byte trace (for wire-level debugging)
 * ================================================================ */

void qws_trace_raw_bytes(int32_t client_id, const void *data, size_t len) {
    if (g_trace_level < QWS_TRACE_HEXDUMP || !data || len == 0)
        return;

    FILE *fp = trace_fp();
    print_timestamp(fp);
    fprintf(fp, "=== recv from client %d (%zd bytes) ===\n", client_id, len);
    qws_trace_hexdump(fp, "  ", data, len);
    fflush(fp);
}