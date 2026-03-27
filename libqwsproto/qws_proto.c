/*
 * qws_proto.c - QWS wire protocol implementation
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include "qws_proto.h"
#include "qws_server_helpers.h"
#include "qws_qdatastream.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ftw.h>
#include <dirent.h>

#include <assert.h>
 
/* ================================================================
 * Packet allocation / free
 * ================================================================ */
 
qws_packet_t *qws_packet_alloc(int32_t type, size_t simple_len, size_t raw_len)
{
    qws_packet_t *pkt = calloc(1, sizeof(*pkt));
    if (!pkt)
        return NULL;
 
    pkt->header.type = type;
    pkt->header.simple_len = (int32_t)simple_len;
    pkt->header.raw_len = (int32_t)raw_len;
 
    if (simple_len > 0) {
        pkt->simple_data = calloc(1, simple_len);
        if (!pkt->simple_data) {
            free(pkt);
            return NULL;
        }
    }
 
    if (raw_len > 0) {
        pkt->raw_data = calloc(1, raw_len);
        if (!pkt->raw_data) {
            free(pkt->simple_data);
            free(pkt);
            return NULL;
        }
    }
 
    return pkt;
}
 
void qws_packet_free(qws_packet_t *pkt)
{
    if (!pkt) return;
    free(pkt->simple_data);
    free(pkt->raw_data);
    free(pkt);
}
 
/* ================================================================
 * Type → simpleData size lookup
 *
 * Separate functions for events and commands since both enums
 * start at 0 (they flow in opposite directions on the wire).
 * ================================================================ */
 
int32_t qws_event_simple_len(int32_t type)
{
    switch (type) {
    case QWS_EVT_NOEVENT:           return 0;
    case QWS_EVT_CONNECTED:         return (int32_t)sizeof(qws_evt_connected_t);
    case QWS_EVT_MOUSE:             return (int32_t)sizeof(qws_evt_mouse_t);
    case QWS_EVT_FOCUS:             return (int32_t)sizeof(qws_evt_focus_t);
    case QWS_EVT_KEY:               return (int32_t)sizeof(qws_evt_key_t);
    case QWS_EVT_REGION:            return (int32_t)sizeof(qws_evt_region_t);
    case QWS_EVT_CREATION:          return (int32_t)sizeof(qws_evt_creation_t);
    case QWS_EVT_PROPERTY_NOTIFY:   return (int32_t)sizeof(qws_evt_property_notify_t);
    case QWS_EVT_PROPERTY_REPLY:    return (int32_t)sizeof(qws_evt_property_reply_t);
    case QWS_EVT_MAX_WINDOW_RECT:   return (int32_t)sizeof(qws_evt_max_window_rect_t);
    case QWS_EVT_WINDOW_OPERATION:  return (int32_t)sizeof(qws_evt_window_operation_t);
    case QWS_EVT_EMBED:             return (int32_t)sizeof(qws_evt_embed_t);
    case QWS_EVT_SELECTION_CLEAR:   return (int32_t)sizeof(qws_evt_selection_clear_t);
    case QWS_EVT_SELECTION_REQUEST: return (int32_t)sizeof(qws_evt_selection_request_t);
    case QWS_EVT_SELECTION_NOTIFY:  return (int32_t)sizeof(qws_evt_selection_notify_t);
    case QWS_EVT_QCOP_MESSAGE:      return (int32_t)sizeof(qws_evt_qcop_message_t);
    case QWS_EVT_IM_EVENT:          return (int32_t)sizeof(qws_evt_im_event_t);
    case QWS_EVT_IM_QUERY:          return (int32_t)sizeof(qws_evt_im_query_t);
    case QWS_EVT_IM_INIT:           return (int32_t)sizeof(qws_evt_im_init_t);
    case QWS_EVT_FONT:              return (int32_t)sizeof(qws_evt_font_t);
    case QWS_EVT_SCREEN_TRANSFORM:  return (int32_t)sizeof(qws_evt_screen_transform_t);
    default:                         return -1;
    }
}
 
int32_t qws_command_simple_len(int32_t type)
{
    switch (type) {
    case QWS_CMD_CREATE:             return (int32_t)sizeof(qws_cmd_create_t);
    case QWS_CMD_SHUTDOWN:           return (int32_t)sizeof(qws_cmd_shutdown_t);
    case QWS_CMD_REGION:             return (int32_t)sizeof(qws_cmd_region_request_t);
    case QWS_CMD_REGION_MOVE:        return (int32_t)sizeof(qws_cmd_region_move_t);
    case QWS_CMD_REGION_DESTROY:     return (int32_t)sizeof(qws_cmd_region_destroy_t);
    case QWS_CMD_SET_PROPERTY:       return (int32_t)sizeof(qws_cmd_set_property_t);
    case QWS_CMD_ADD_PROPERTY:       return (int32_t)sizeof(qws_cmd_add_property_t);
    case QWS_CMD_REMOVE_PROPERTY:    return (int32_t)sizeof(qws_cmd_remove_property_t);
    case QWS_CMD_GET_PROPERTY:       return (int32_t)sizeof(qws_cmd_get_property_t);
    case QWS_CMD_SET_SELECTION_OWNER:return (int32_t)sizeof(qws_cmd_set_selection_owner_t);
    case QWS_CMD_CONVERT_SELECTION:  return (int32_t)sizeof(qws_cmd_convert_selection_t);
    case QWS_CMD_REQUEST_FOCUS:      return (int32_t)sizeof(qws_cmd_request_focus_t);
    case QWS_CMD_CHANGE_ALTITUDE:    return (int32_t)sizeof(qws_cmd_change_altitude_t);
    case QWS_CMD_SET_OPACITY:        return (int32_t)sizeof(qws_cmd_set_opacity_t);
    case QWS_CMD_DEFINE_CURSOR:      return (int32_t)sizeof(qws_cmd_define_cursor_t);
    case QWS_CMD_SELECT_CURSOR:      return (int32_t)sizeof(qws_cmd_select_cursor_t);
    case QWS_CMD_POSITION_CURSOR:    return (int32_t)sizeof(qws_cmd_position_cursor_t);
    case QWS_CMD_GRAB_MOUSE:         return (int32_t)sizeof(qws_cmd_grab_mouse_t);
    case QWS_CMD_PLAY_SOUND:         return (int32_t)sizeof(qws_cmd_play_sound_t);
    case QWS_CMD_QCOP_REGISTER:      return (int32_t)sizeof(qws_cmd_qcop_register_t);
    case QWS_CMD_QCOP_SEND:          return (int32_t)sizeof(qws_cmd_qcop_send_t);
    case QWS_CMD_REGION_NAME:        return (int32_t)sizeof(qws_cmd_region_name_t);
    case QWS_CMD_IDENTIFY:           return (int32_t)sizeof(qws_cmd_identify_t);
    case QWS_CMD_GRAB_KEYBOARD:      return (int32_t)sizeof(qws_cmd_grab_keyboard_t);
    case QWS_CMD_REPAINT_REGION:     return (int32_t)sizeof(qws_cmd_repaint_region_t);
    case QWS_CMD_IM_MOUSE:           return (int32_t)sizeof(qws_cmd_im_mouse_t);
    case QWS_CMD_IM_UPDATE:          return (int32_t)sizeof(qws_cmd_im_update_t);
    case QWS_CMD_IM_RESPONSE:        return (int32_t)sizeof(qws_cmd_im_response_t);
    case QWS_CMD_EMBED:              return (int32_t)sizeof(qws_cmd_embed_t);
    case QWS_CMD_FONT:               return (int32_t)sizeof(qws_cmd_font_t);
    case QWS_CMD_SCREEN_TRANSFORM:   return (int32_t)sizeof(qws_cmd_screen_transform_t);
    default:                          return -1;
    }
}
 
/* ================================================================
 * Serialization
 *
 * Wire format: [type:4][rawLen:4][simpleData:simple_len][rawData:rawLen]
 * ================================================================ */
 
size_t qws_packet_wire_size(const qws_packet_t *pkt)
{
    return QWS_WIRE_HEADER_SIZE
         + (size_t)pkt->header.simple_len
         + (size_t)pkt->header.raw_len;
}
 
size_t qws_packet_serialize(const qws_packet_t *pkt, void *buf, size_t buflen)
{
    size_t needed = qws_packet_wire_size(pkt);
    if (buflen < needed)
        return 0;
 
    uint8_t *p = (uint8_t *)buf;
 
    /* type */
    memcpy(p, &pkt->header.type, 4);
    p += 4;
 
    /* rawLen */
    memcpy(p, &pkt->header.raw_len, 4);
    p += 4;
 
    /* simpleData (fixed size, known from type — no length prefix) */
    if (pkt->header.simple_len > 0 && pkt->simple_data) {
        memcpy(p, pkt->simple_data, pkt->header.simple_len);
        p += pkt->header.simple_len;
    }
 
    /* rawData */
    if (pkt->header.raw_len > 0 && pkt->raw_data) {
        memcpy(p, pkt->raw_data, pkt->header.raw_len);
        p += pkt->header.raw_len;
    }
 
    return (size_t)(p - (uint8_t *)buf);
}
 
/* ================================================================
 * Stream reader
 *
 * Wire format: [type:4][rawLen:4][simpleData:N][rawData:rawLen]
 * where N = qws_simple_len_for_type(type), known after reading the header.
 * ================================================================ */
 
void qws_reader_init(qws_reader_t *r, bool reading_commands)
{
    bool saved = r->reading_commands;
    memset(r, 0, sizeof(*r));
    r->state = QWS_READ_HEADER;
    /* If called for the first time, use the argument;
     * if called from reset, preserve the existing direction */
    r->reading_commands = reading_commands || saved;
}
 
void qws_reader_reset(qws_reader_t *r)
{
    bool saved = r->reading_commands;
    free(r->simple_buf);
    free(r->raw_buf);
    memset(r, 0, sizeof(*r));
    r->state = QWS_READ_HEADER;
    r->reading_commands = saved;
}
 
size_t qws_reader_feed(qws_reader_t *r, const void *data, size_t len,
                        qws_packet_t **out)
{
    const uint8_t *src = (const uint8_t *)data;
    size_t consumed = 0;
    *out = NULL;
 
    while (consumed < len && r->state != QWS_READ_DONE) {
        size_t avail = len - consumed;
 
        switch (r->state) {
 
        case QWS_READ_HEADER: {
            /* Read 8 bytes: type(4) + rawLen(4) */
            size_t need = QWS_WIRE_HEADER_SIZE - r->bytes_read;
            size_t take = (avail < need) ? avail : need;
            memcpy(((uint8_t *)&r->hdr) + r->bytes_read, src + consumed, take);
            r->bytes_read += take;
            consumed += take;
 
            if (r->bytes_read == QWS_WIRE_HEADER_SIZE) {
                /* Header complete — derive simple_len from type and direction */
                int32_t slen = r->reading_commands
                    ? qws_command_simple_len(r->hdr.type)
                    : qws_event_simple_len(r->hdr.type);
                r->hdr.simple_len = (slen > 0) ? slen : 0;
                r->bytes_read = 0;
 
                if (r->hdr.simple_len > 0) {
                    r->simple_buf = calloc(1, r->hdr.simple_len);
                    if (!r->simple_buf) {
                        qws_reader_reset(r);
                        return consumed;
                    }
                    r->state = QWS_READ_SIMPLE;
                } else if (r->hdr.raw_len > 0) {
                    r->raw_buf = calloc(1, r->hdr.raw_len);
                    if (!r->raw_buf) {
                        qws_reader_reset(r);
                        return consumed;
                    }
                    r->state = QWS_READ_RAW;
                } else {
                    r->state = QWS_READ_DONE;
                }
            }
            break;
        }
 
        case QWS_READ_SIMPLE: {
            size_t need = (size_t)r->hdr.simple_len - r->bytes_read;
            size_t take = (avail < need) ? avail : need;
            memcpy(r->simple_buf + r->bytes_read, src + consumed, take);
            r->bytes_read += take;
            consumed += take;
 
            if (r->bytes_read == (size_t)r->hdr.simple_len) {
                r->bytes_read = 0;
                if (r->hdr.raw_len > 0) {
                    r->raw_buf = calloc(1, r->hdr.raw_len);
                    if (!r->raw_buf) {
                        qws_reader_reset(r);
                        return consumed;
                    }
                    r->state = QWS_READ_RAW;
                } else {
                    r->state = QWS_READ_DONE;
                }
            }
            break;
        }
 
        case QWS_READ_RAW: {
            size_t need = (size_t)r->hdr.raw_len - r->bytes_read;
            size_t take = (avail < need) ? avail : need;
            memcpy(r->raw_buf + r->bytes_read, src + consumed, take);
            r->bytes_read += take;
            consumed += take;
 
            if (r->bytes_read == (size_t)r->hdr.raw_len) {
                r->state = QWS_READ_DONE;
            }
            break;
        }
 
        case QWS_READ_DONE:
            break;
        }
    }
 
    /* Assemble completed packet */
    if (r->state == QWS_READ_DONE) {
        qws_packet_t *pkt = calloc(1, sizeof(*pkt));
        if (pkt) {
            pkt->header = r->hdr;
            pkt->simple_data = r->simple_buf;
            pkt->raw_data = r->raw_buf;
            *out = pkt;
        } else {
            free(r->simple_buf);
            free(r->raw_buf);
        }
        /* Reset for next packet (bufs are now owned by pkt) */
        r->simple_buf = NULL;
        r->raw_buf = NULL;
        qws_reader_reset(r);
    }
 
    return consumed;
}

/* ================================================================
 * Display directory paths
 * ================================================================ */

int qws_display_paths_fill(int display, qws_display_paths_t *paths)
{
    if (snprintf(paths->base, sizeof(paths->base),
            "/tmp/qtembedded-%d", display) >= (int)sizeof(paths->base))
        return -1;

    if (snprintf(paths->socket, sizeof(paths->socket), "%s/QtEmbedded-%d",
            paths->base, display) >= (int)sizeof(paths->socket))
        return -1;

    if (snprintf(paths->fonts,  sizeof(paths->fonts),  "%s/fonts",
            paths->base) >= (int)sizeof(paths->fonts))
        return -1;

    if (snprintf(paths->fontdb, sizeof(paths->fontdb), "%s/fontdb", 
            paths->fonts) >= (int)sizeof(paths->fontdb))
        return -1;
    
    return 0;
}

static int rm_entry_cb(const char *path, const struct stat *st,
                        int typeflag, struct FTW *ftwbuf)
{
    (void)st; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

int qws_init_display_dir(int display, qws_display_paths_t *paths)
{
    if (qws_display_paths_fill(display, paths) != 0)
        return -1;

    /* Remove any stale tree; ignore error if it didn't exist. */
    nftw(paths->base, rm_entry_cb, 16, FTW_DEPTH | FTW_PHYS);

    if (mkdir(paths->base, 0600) < 0)
        return -1;

    if (mkdir(paths->fonts, 0600) < 0)
        return -1;

    return 0;
}

/* ================================================================
 * Font database handling
 * ================================================================ */

/* Parse a QPF filename into font metadata.
 * Format: familyname_pixelsize*10_weight[i].qpf
 * Returns 0 on success, -1 if the filename does not match the pattern. */
static int parse_qpf_filename(const char *basename,
                               char *family, size_t family_sz,
                               int *pixelsize, int *weight, bool *italic)
{
    const char *u0 = strchr(basename, '_');
    if (!u0) return -1;
    const char *u1 = strchr(u0 + 1, '_');
    if (!u1) return -1;

    *pixelsize = atoi(u0 + 1) / 10;

    /* u2 = next '_' after u1, or '.' if there is none */
    const char *u2 = strchr(u1 + 1, '_');
    const char *u3 = strchr(u1 + 1, '.');
    if (!u2) u2 = u3;
    if (!u2) return -1;

    *italic = (u2 > u1 + 1) && (*(u2 - 1) == 'i');

    size_t wlen = (size_t)(u2 - u1 - 1) - (*italic ? 1 : 0);
    char wbuf[16];
    if (wlen == 0 || wlen >= sizeof(wbuf)) return -1;
    memcpy(wbuf, u1 + 1, wlen);
    wbuf[wlen] = '\0';
    *weight = atoi(wbuf);

    size_t flen = (size_t)(u0 - basename);
    if (flen == 0 || flen >= family_sz) return -1;
    memcpy(family, basename, flen);
    family[flen] = '\0';
    return 0;
}

/* Write one QDataStream font entry (see qfontdatabase_qws.cpp cache format). */
static int write_font_entry(FILE *f,
                             const char *family, const char *foundry,
                             int32_t weight, bool italic, int32_t pixelsize,
                             const char *filepath)
{
    return qws_fwrite_qdatastream_qstring(f, family)                          /* familyname      */
        || qws_fwrite_qdatastream_qstring(f, foundry)                         /* foundryname     */
        || qws_fwrite_qdatastream_int32(f, weight)                            /* weight          */
        || qws_fwrite_qdatastream_uint8(f, italic ? 1u : 0u)                  /* italic          */
        || qws_fwrite_qdatastream_int32(f, pixelsize)                         /* pixelSize       */
        || qws_fwrite_qdatastream_bytearray(f, filepath, (int32_t)strlen(filepath)) /* file      */
        || qws_fwrite_qdatastream_int32(f, 0)                                 /* fileIndex       */
        || qws_fwrite_qdatastream_uint8(f, 1u)                                /* antialiased     */
        || qws_fwrite_qdatastream_uint8(f, 0u);                               /* writingSystemCount=0 */
}

int qws_build_font_database(const char *fontdb_path)
{
    FILE *f = fopen(fontdb_path, "wb");
    if (!f)
        return -1;

    /* Two header bytes: cache version + QDataStream version */
    uint8_t hdr[2] = { QWS_FONTDB_VERSION, QWS_FONTDB_DATASTREAM_VERSION };
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        goto err;

    /* QDataStream QString: font directory path */
    if (qws_fwrite_qdatastream_qstring(f, QWS_FONTDB_FONT_PATH) != 0)
        goto err;

    /* Scan font directory for *.qpf files and write one entry per font */
    DIR *dir = opendir(QWS_FONTDB_FONT_PATH);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *name = de->d_name;
            size_t nlen = strlen(name);
            if (nlen < 5 || strcmp(name + nlen - 4, ".qpf") != 0)
                continue;

            char family[128];
            int pixelsize, weight;
            bool italic;
            if (parse_qpf_filename(name, family, sizeof(family),
                                   &pixelsize, &weight, &italic) != 0)
                continue;

            char abspath[PATH_MAX];
            if (snprintf(abspath, sizeof(abspath), "%s/%s",
                         QWS_FONTDB_FONT_PATH, name) >= (int)sizeof(abspath))
                continue;

            if (write_font_entry(f, family, "qt", weight, italic,
                                 pixelsize, abspath) != 0) {
                closedir(dir);
                goto err;
            }
        }
        closedir(dir);
    }

    /* Null-QString terminator (-1) signals end of font list */
    if (qws_fwrite_qdatastream_int32(f, -1) != 0)
        goto err;

    /* Empty fallback-families QStringList (count = 0) */
    if (qws_fwrite_qdatastream_int32(f, 0) != 0)
        goto err;

    fclose(f);
    return 0;

err:
    fclose(f);
    return -1;
}
 
/* ================================================================
 * Socket transport
 * ================================================================ */
 
int qws_server_listen(const char *socket_path)
{
    if (!socket_path)
        return -1;

    /* Remove stale socket */
    unlink(socket_path);
 
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
 
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
 
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
 
    /* Restrict permissions */
    chmod(socket_path, 0700);
 
    if (listen(fd, 16) < 0) {
        close(fd);
        unlink(socket_path);
        return -1;
    }
 
    return fd;
}
 
int qws_server_accept(int server_fd)
{
    struct sockaddr_un addr;
    socklen_t len = sizeof(addr);
    return accept(server_fd, (struct sockaddr *)&addr, &len);
}
 
int qws_client_connect(const char *socket_path)
{
    if (!socket_path)
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int qws_write_packet(int fd, const qws_packet_t *pkt)
{
    size_t total = qws_packet_wire_size(pkt);
    uint8_t *buf = malloc(total);
    if (!buf)
        return -1;
 
    qws_packet_serialize(pkt, buf, total);
 
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(fd, buf + written, total - written);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            free(buf);
            return -1;
        }
        written += (size_t)n;
    }
 
    free(buf);
    return 0;
}
 
/* ================================================================
 * Shared memory
 * ================================================================ */
#ifdef QWS_IPC_POSIX
static int qws_shm_create_ipc(qws_shm_t *shm, size_t size) {
    char shm_name[128];
    void *p;
    int fd;
    
    // TODO: should be improved?
    snprintf(shm_name, sizeof(shm_name), "qwslib-%d-%p", getpid(), shm);

    fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    
    if (ftruncate(fd, (off_t)size) < 0)
        goto error;

    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        goto error;

    shm->fd = fd;
    shm->base = p;
    
    return 0;
error:
    close(fd);
    shm_unlink(shm_name);
    return -1;
}

#else

static int qws_shm_create_sysv(qws_shm_t *shm, size_t size) {
    int id = shmget(IPC_PRIVATE, size, IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0)
        return -1;

    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) {
        shmctl(id, IPC_RMID, NULL);
        return -1;
    }

    shm->shm_id = id;
    shm->base = p;

    return 0;
}

#endif

int qws_shm_create(qws_shm_t *shm, size_t size)
{
    memset(shm, 0, sizeof(*shm));
    shm->shm_id = -1;
    shm->fd = -1;
    shm->size = size;

#ifdef QWS_IPC_POSIX
    return qws_shm_create_ipc(shm, size);
#else
    return qws_shm_create_sysv(shm, size);
#endif
}

void qws_shm_destroy(qws_shm_t *shm)
{
    if (!shm)
        return;

    if (shm->base) {
        if (shm->fd >= 0) {
            munmap(shm->base, shm->size);
            close(shm->fd);
        } else if (shm->shm_id >= 0) {
            shmdt(shm->base);
            shmctl(shm->shm_id, IPC_RMID, NULL);
        }
    }

    memset(shm, 0, sizeof(*shm));
    shm->shm_id = -1;
    shm->fd = -1;
}

int qws_shm_attach_sysv(qws_shm_t *shm, int shm_id)
{
    memset(shm, 0, sizeof(*shm));
    shm->fd = -1;
    shm->shm_id = shm_id;
    struct shmid_ds ds;
    if (shmctl(shm_id, IPC_STAT, &ds) < 0)
        return -1;

    shm->base = shmat(shm_id, NULL, SHM_RDONLY);
    if (shm->base == (void *)-1) {
        shm->base = NULL;
        shm->shm_id = -1;
        shm->size = -1;
        return -1;
    }

    shm->size = ds.shm_segsz;
    return 0;
}

int qws_shm_update_sysv(qws_shm_t *shm)
{
    struct shmid_ds ds;
    if (shmctl(shm->shm_id, IPC_STAT, &ds) < 0)
        return -1;

    shm->size = ds.shm_segsz;
    return 0;
}

void qws_shm_detach(qws_shm_t *shm)
{
    if (shm->base && shm->shm_id >= 0)
        shmdt(shm->base);
    memset(shm, 0, sizeof(*shm));
    shm->shm_id = -1;
    shm->fd = -1;
}

/* ================================================================
 * Debug helpers
 * ================================================================ */

const char *qws_event_type_name(int type)
{
    switch (type) {
    case QWS_EVT_NOEVENT:           return "NoEvent";
    case QWS_EVT_CONNECTED:         return "Connected";
    case QWS_EVT_MOUSE:             return "Mouse";
    case QWS_EVT_FOCUS:             return "Focus";
    case QWS_EVT_KEY:               return "Key";
    case QWS_EVT_REGION:            return "Region";
    case QWS_EVT_CREATION:          return "Creation";
    case QWS_EVT_PROPERTY_NOTIFY:   return "PropertyNotify";
    case QWS_EVT_PROPERTY_REPLY:    return "PropertyReply";
    case QWS_EVT_SELECTION_CLEAR:   return "SelectionClear";
    case QWS_EVT_SELECTION_REQUEST: return "SelectionRequest";
    case QWS_EVT_SELECTION_NOTIFY:  return "SelectionNotify";
    case QWS_EVT_MAX_WINDOW_RECT:   return "MaxWindowRect";
    case QWS_EVT_QCOP_MESSAGE:      return "QCopMessage";
    case QWS_EVT_WINDOW_OPERATION:  return "WindowOperation";
    case QWS_EVT_IM_EVENT:          return "IMEvent";
    case QWS_EVT_IM_QUERY:          return "IMQuery";
    case QWS_EVT_IM_INIT:          return "IMInit";
    case QWS_EVT_EMBED:             return "Embed";
    case QWS_EVT_FONT:              return "Font";
    case QWS_EVT_SCREEN_TRANSFORM:  return "ScreenTransformation";
    default:                         return "Unknown";
    }
}

 
const char *qws_command_type_name(int type)
{
    switch (type) {
    case QWS_CMD_UNKNOWN:            return "Unknown";
    case QWS_CMD_CREATE:             return "Create";
    case QWS_CMD_SHUTDOWN:           return "Shutdown";
    case QWS_CMD_REGION:             return "Region";
    case QWS_CMD_REGION_MOVE:        return "RegionMove";
    case QWS_CMD_REGION_DESTROY:     return "RegionDestroy";
    case QWS_CMD_SET_PROPERTY:       return "SetProperty";
    case QWS_CMD_ADD_PROPERTY:       return "AddProperty";
    case QWS_CMD_REMOVE_PROPERTY:    return "RemoveProperty";
    case QWS_CMD_GET_PROPERTY:       return "GetProperty";
    case QWS_CMD_SET_SELECTION_OWNER:return "SetSelectionOwner";
    case QWS_CMD_CONVERT_SELECTION:  return "ConvertSelection";
    case QWS_CMD_REQUEST_FOCUS:      return "RequestFocus";
    case QWS_CMD_CHANGE_ALTITUDE:    return "ChangeAltitude";
    case QWS_CMD_SET_OPACITY:        return "SetOpacity";
    case QWS_CMD_DEFINE_CURSOR:      return "DefineCursor";
    case QWS_CMD_SELECT_CURSOR:      return "SelectCursor";
    case QWS_CMD_POSITION_CURSOR:    return "PositionCursor";
    case QWS_CMD_GRAB_MOUSE:         return "GrabMouse";
    case QWS_CMD_PLAY_SOUND:         return "PlaySound";
    case QWS_CMD_QCOP_REGISTER:      return "QCopRegisterChannel";
    case QWS_CMD_QCOP_SEND:          return "QCopSend";
    case QWS_CMD_REGION_NAME:        return "RegionName";
    case QWS_CMD_IDENTIFY:           return "Identify";
    case QWS_CMD_GRAB_KEYBOARD:      return "GrabKeyboard";
    case QWS_CMD_REPAINT_REGION:     return "RepaintRegion";
    case QWS_CMD_IM_MOUSE:           return "IMMouse";
    case QWS_CMD_IM_UPDATE:          return "IMUpdate";
    case QWS_CMD_IM_RESPONSE:        return "IMResponse";
    case QWS_CMD_EMBED:              return "Embed";
    case QWS_CMD_FONT:               return "Font";
    case QWS_CMD_SCREEN_TRANSFORM:   return "ScreenTransform";
    default:                          return "Unknown";
    }
}

const char *qws_surface_flag_name(int flag)
{
    switch (flag) {
    case QWS_SURFACE_REGION_RESERVED: return "RegionReserved";
    case QWS_SURFACE_BUFFERED:        return "Buffered";
    case QWS_SURFACE_OPAQUE:          return "Opaque";
    default:                           return "Unknown";
    }
}

const char *qws_im_update_type_name(int type)
{
    switch (type) {
    case QWS_IM_FOCUS_IN:  return "FocusIn";
    case QWS_IM_FOCUS_OUT: return "FocusOut";
    case QWS_IM_UPDATE:    return "Update";
    case QWS_IM_DICTATION: return "Dictation";
    default:               return "Unknown";
    }
}

const char *qws_altitude_name(int altitude)
{
    switch (altitude) {
    case QWS_ALTITUDE_LOWER:        return "Lower";
    case QWS_ALTITUDE_RAISE:        return "Raise";
    case QWS_ALTITUDE_STAYS_ON_TOP: return "StaysOnTop";
    default:                        return "Unknown";
    }
}

bool qws_is_synchronous_commmand(int type)
{
    switch(type){
    case QWS_CMD_CHANGE_ALTITUDE:
    case QWS_CMD_REGION:
    case QWS_CMD_REPAINT_REGION:
    case QWS_CMD_REGION_MOVE:
        return true;
    }
    return false;
}

const char *qws_image_format_name(int format)
{
    switch (format) {
    case QWS_FORMAT_INVALID:                return "Invalid";
    case QWS_FORMAT_MONO:                   return "Mono";
    case QWS_FORMAT_MONO_LSB:               return "MonoLSB";
    case QWS_FORMAT_INDEXED8:               return "Indexed8";
    case QWS_FORMAT_RGB32:                  return "RGB32";
    case QWS_FORMAT_ARGB32:                 return "ARGB32";
    case QWS_FORMAT_ARGB32_PREMULTIPLIED:   return "ARGB32_Premultiplied";
    case QWS_FORMAT_RGB16:                  return "RGB16";
    case QWS_FORMAT_ARGB8565_PREMULTIPLIED: return "ARGB8565_Premultiplied";
    case QWS_FORMAT_RGB666:                 return "RGB666";
    case QWS_FORMAT_ARGB6666_PREMULTIPLIED: return "ARGB6666_Premultiplied";
    case QWS_FORMAT_RGB555:                 return "RGB555";
    case QWS_FORMAT_ARGB8555_PREMULTIPLIED: return "ARGB8555_Premultiplied";
    case QWS_FORMAT_RGB888:                 return "RGB888";
    case QWS_FORMAT_RGB444:                 return "RGB444";
    case QWS_FORMAT_ARGB4444_PREMULTIPLIED: return "ARGB4444_Premultiplied";
    default:                                return "Unknown";
    }
}

const char *qws_window_type_str(uint32_t flags)
{
    switch (flags & QWS_WINDOW_TYPE_MASK) {
        case QWS_WT_WIDGET:       return "Widget";
        case QWS_WT_WINDOW:       return "Window";
        case QWS_WT_DIALOG:       return "Dialog";
        case QWS_WT_SHEET:        return "Sheet";
        case QWS_WT_DRAWER:       return "Drawer";
        case QWS_WT_POPUP:        return "Popup";
        case QWS_WT_TOOL:         return "Tool";
        case QWS_WT_TOOLTIP:      return "ToolTip";
        case QWS_WT_SPLASHSCREEN: return "SplashScreen";
        case QWS_WT_DESKTOP:      return "Desktop";
        case QWS_WT_SUBWINDOW:    return "SubWindow";
        default:                  return "Unknown";
    }
}

const char *qws_focus_flag_str(qws_focus_flag_t flag)
{
    switch (flag) {
        case QWS_FOCUS_LOSE: return "Lose";
        case QWS_FOCUS_GAIN: return "Gain";
        default:             return "Invalid";
    }
}