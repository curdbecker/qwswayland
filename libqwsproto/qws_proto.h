/*
 * qws_proto.h - Clean-room QWS wire protocol definitions
 *
 * This is a standalone reimplementation of the QWS (Qt Window System)
 * protocol as used by Qt 4.8.x/Embedded Linux, based on the publicly
 * documented behavior and API of QWSEvent, QWSServer, and QWSClient.
 *
 * No Qt source code is included or linked. The wire format was derived
 * from:
 *   - Qt 4.8 public API documentation for QWSEvent::Type enum
 *   - Qt 4.8 public API documentation for QWSServer, QWSClient, QWSWindow
 *   - Public struct layout references (Doxygen cross-references)
 *   - Black-box protocol analysis
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_PROTO_H
#define QWS_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 * Wire format overview
 * ============================================================
 *
 * From qwscommand_qws.cpp:
 *
 *   [type:int32] [rawLen:int32] [simpleData] [rawData:rawLen bytes]
 *
 * "type" identifies the command or event.
 * "rawLen" is the length of the variable-length payload.
 * "simpleData" is a fixed-size struct whose size is known from the type —
 *   it is NOT preceded by a length field on the wire.
 * "rawData" is variable-length payload (e.g., region rectangles,
 *  property values, strings, etc.)
 *
 * Commands flow client→server. Events flow server→client.
 * Both use the same framing.
 *
 * The protocol is native-endian (both sides are the same machine).
 * All ints are 32-bit. Coordinates are int32. Booleans are int32.
 */
 
/* -----------------------------------------------------------
 * Packet header (as it appears on the wire: 8 bytes)
 * ----------------------------------------------------------- */
 
typedef struct {
    int32_t type;        /* QWS_CMD_* or QWS_EVT_* */
    int32_t raw_len;     /* length of the variable payload, or 0 */
    int32_t simple_len;  /* NOT on wire — derived from type, stored for convenience */
} qws_packet_header_t;
 
#define QWS_WIRE_HEADER_SIZE 8  /* only type + raw_len go on the wire */
 
/* -----------------------------------------------------------
 * QWS Event types (server → client)
 * Values match QWSEvent::Type enum in Qt 4.8
 * ----------------------------------------------------------- */
 
enum qws_event_type {
    QWS_EVT_NOEVENT             = 0,
    QWS_EVT_CONNECTED           = 1,
    QWS_EVT_MOUSE               = 2,
    QWS_EVT_FOCUS               = 3,
    QWS_EVT_KEY                 = 4,
    QWS_EVT_REGION              = 5,
    QWS_EVT_CREATION            = 6,
    QWS_EVT_PROPERTY_NOTIFY     = 7,
    QWS_EVT_PROPERTY_REPLY      = 8,
    QWS_EVT_SELECTION_CLEAR     = 9,
    QWS_EVT_SELECTION_REQUEST   = 10,
    QWS_EVT_SELECTION_NOTIFY    = 11,
    QWS_EVT_MAX_WINDOW_RECT     = 12,
    QWS_EVT_QCOP_MESSAGE        = 13,
    QWS_EVT_WINDOW_OPERATION    = 14,
    QWS_EVT_IM_EVENT            = 15,
    QWS_EVT_IM_QUERY            = 16,
    QWS_EVT_IM_INIT             = 17,
    QWS_EVT_EMBED               = 18,
    QWS_EVT_FONT                = 19,
    QWS_EVT_SCREEN_TRANSFORM    = 20,
    QWS_EVT_NEVENT              = 21,  /* sentinel */
};

/* -----------------------------------------------------------
 * QWS Command types (client → server)
 * Values derived from QWSCommand::Type in Qt 4.8
 * The command enum starts where events leave off,
 * using a base offset of 0x100.
 * ----------------------------------------------------------- */
 
enum qws_command_type {
    /*
     * Exact match of QWSCommand::Type from qwscommand_qws_p.h.
     * Commands flow client→server, events flow server→client.
     * Both enums start at 0 independently.
     */
    QWS_CMD_UNKNOWN             = 0,
    QWS_CMD_CREATE              = 1,
    QWS_CMD_SHUTDOWN            = 2,
    QWS_CMD_REGION              = 3,   /* aka RegionRequest */
    QWS_CMD_REGION_MOVE         = 4,
    QWS_CMD_REGION_DESTROY      = 5,
    QWS_CMD_SET_PROPERTY        = 6,
    QWS_CMD_ADD_PROPERTY        = 7,
    QWS_CMD_REMOVE_PROPERTY     = 8,
    QWS_CMD_GET_PROPERTY        = 9,
    QWS_CMD_SET_SELECTION_OWNER = 10,
    QWS_CMD_CONVERT_SELECTION   = 11,
    QWS_CMD_REQUEST_FOCUS       = 12,
    QWS_CMD_CHANGE_ALTITUDE     = 13,
    QWS_CMD_SET_OPACITY         = 14,
    QWS_CMD_DEFINE_CURSOR       = 15,
    QWS_CMD_SELECT_CURSOR       = 16,
    QWS_CMD_POSITION_CURSOR     = 17,
    QWS_CMD_GRAB_MOUSE          = 18,
    QWS_CMD_PLAY_SOUND          = 19,
    QWS_CMD_QCOP_REGISTER       = 20,
    QWS_CMD_QCOP_SEND           = 21,
    QWS_CMD_REGION_NAME         = 22,
    QWS_CMD_IDENTIFY            = 23,
    QWS_CMD_GRAB_KEYBOARD       = 24,
    QWS_CMD_REPAINT_REGION      = 25,
    QWS_CMD_IM_MOUSE            = 26,
    QWS_CMD_IM_UPDATE           = 27,
    QWS_CMD_IM_RESPONSE         = 28,
    QWS_CMD_EMBED               = 29,
    QWS_CMD_FONT                = 30,
    QWS_CMD_SCREEN_TRANSFORM    = 31,
};
 
/* -----------------------------------------------------------
 * QImage pixel format (QImage::Format enum, Qt 4.8)
 * Used in qws_cmd_region_surface_data_shm_t::format
 * ----------------------------------------------------------- */

enum qws_image_format {
    QWS_FORMAT_INVALID                  = 0,
    QWS_FORMAT_MONO                     = 1,
    QWS_FORMAT_MONO_LSB                 = 2,
    QWS_FORMAT_INDEXED8                 = 3,
    QWS_FORMAT_RGB32                    = 4,
    QWS_FORMAT_ARGB32                   = 5,
    QWS_FORMAT_ARGB32_PREMULTIPLIED     = 6,
    QWS_FORMAT_RGB16                    = 7,
    QWS_FORMAT_ARGB8565_PREMULTIPLIED   = 8,
    QWS_FORMAT_RGB666                   = 9,
    QWS_FORMAT_ARGB6666_PREMULTIPLIED   = 10,
    QWS_FORMAT_RGB555                   = 11,
    QWS_FORMAT_ARGB8555_PREMULTIPLIED   = 12,
    QWS_FORMAT_RGB888                   = 13,
    QWS_FORMAT_RGB444                   = 14,
    QWS_FORMAT_ARGB4444_PREMULTIPLIED   = 15,
    QWS_FORMAT_NFORMATS                 = 16,  /* sentinel */
};

/* Qt::WindowType values (from qnamespace.h, Qt 4.8) */
#define QWS_WINDOW_TYPE_MASK            0x000000ffu

#define QWS_WT_WIDGET                   0x00000000u
#define QWS_WT_WINDOW                   0x00000001u
#define QWS_WT_DIALOG                   0x00000003u   /* 0x02 | Window */
#define QWS_WT_SHEET                    0x00000005u   /* 0x04 | Window */
#define QWS_WT_DRAWER                   0x00000007u   /* 0x06 | Window */
#define QWS_WT_POPUP                    0x00000009u   /* 0x08 | Window */
#define QWS_WT_TOOL                     0x0000000bu   /* 0x0a | Window */
#define QWS_WT_TOOLTIP                  0x0000000du   /* 0x0c | Window */
#define QWS_WT_SPLASHSCREEN             0x0000000fu   /* 0x0e | Window */
#define QWS_WT_DESKTOP                  0x00000011u   /* 0x10 | Window */
#define QWS_WT_SUBWINDOW                0x00000012u

/* Qt::WindowFlags hint bits */
#define QWS_WF_MSWINDOWS_FIXED_SIZE     0x00000100u
#define QWS_WF_MSWINDOWS_OWN_DC        0x00000200u
#define QWS_WF_X11_BYPASS_WM           0x00000400u
#define QWS_WF_FRAMELESS               0x00000800u
#define QWS_WF_TITLE                   0x00001000u
#define QWS_WF_SYSTEM_MENU             0x00002000u
#define QWS_WF_MINIMIZE_BUTTON         0x00004000u
#define QWS_WF_MAXIMIZE_BUTTON         0x00008000u
#define QWS_WF_CONTEXT_HELP_BUTTON     0x00010000u
#define QWS_WF_SHADE_BUTTON            0x00020000u
#define QWS_WF_STAYS_ON_TOP            0x00040000u
#define QWS_WF_OK_BUTTON               0x00080000u
#define QWS_WF_CANCEL_BUTTON           0x00100000u
#define QWS_WF_CUSTOMIZE               0x02000000u
#define QWS_WF_STAYS_ON_BOTTOM         0x04000000u
#define QWS_WF_CLOSE_BUTTON            0x08000000u
#define QWS_WF_MAC_TOOLBAR_BUTTON      0x10000000u
#define QWS_WF_BYPASS_GRAPHICS_PROXY   0x20000000u
#define QWS_WF_SOFTKEYS_VISIBLE        0x40000000u
#define QWS_WF_SOFTKEYS_RESPOND        0x80000000u

/* Helper: is this window type a transient/popup surface? */
#define QWS_WINDOW_TYPE(flags)          ((flags) & QWS_WINDOW_TYPE_MASK)
#define QWS_IS_TOPLEVEL_TYPE(flags) \
    (QWS_WINDOW_TYPE(flags) == QWS_WT_WINDOW || \
     QWS_WINDOW_TYPE(flags) == QWS_WT_DIALOG || \
     QWS_WINDOW_TYPE(flags) == QWS_WT_SHEET  || \
     QWS_WINDOW_TYPE(flags) == QWS_WT_DRAWER)

/* -----------------------------------------------------------
 * QWSWindowSurface flags (QWSWindowSurface::SurfaceFlag, Qt 4.8)
 * Used in qws_cmd_region_surface_data_shm_t::flags (bitmask)
 * ----------------------------------------------------------- */

enum qws_surface_flag {
    QWS_SURFACE_REGION_RESERVED = 0x1,
    QWS_SURFACE_BUFFERED        = 0x2,
    QWS_SURFACE_OPAQUE          = 0x4,
};

/* -----------------------------------------------------------
 * Rectangle (matches QRect wire format: x, y, w, h as int32)
 * ----------------------------------------------------------- */
 
typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} qws_rect_t;
 
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} qwswl_geometry_t;

/* -----------------------------------------------------------
 * Event payloads (server → client simpleData structs)
 * ----------------------------------------------------------- */
 
/* QWS_EVT_CONNECTED: sent once after client connects.
 * Matches QWSConnectedEvent::SimpleData:
 *   { int window, int len, int clientId, int servershmid }
 * rawData = display_spec string (e.g., ":0") */
typedef struct {
    int32_t window;         /* always 0 */
    int32_t len;            /* length of display_spec string in rawData */
    int32_t client_id;
    int32_t server_shm_id;  /* SysV shm id for display properties region */
} qws_evt_connected_t;
 
/* QWS_EVT_MOUSE */
typedef struct {
    int32_t window;     /* target window id */
    int32_t x_root;     /* global x */
    int32_t y_root;     /* global y */
    int32_t state;      /* Qt::MouseButtons | Qt::KeyboardModifiers */
    int32_t delta;
    int32_t time;       /* timestamp ms */
} qws_evt_mouse_t;
 
/* Qt::KeyboardModifier flags — matches Qt 4.8 qnamespace.h */
enum qws_keyboard_modifier {
    QWS_MOD_NONE         = 0x00000000,
    QWS_MOD_SHIFT        = 0x02000000,
    QWS_MOD_CONTROL      = 0x04000000,
    QWS_MOD_ALT          = 0x08000000,
    QWS_MOD_META         = 0x10000000,
    QWS_MOD_KEYPAD       = 0x20000000,
    QWS_MOD_GROUP_SWITCH = 0x40000000,
    QWS_MOD_MASK         = 0xfe000000,
};

/* Bit flags packed into qws_evt_key_t::flags */
enum qws_key_flag {
    QWS_KEY_FLAG_PRESS       = 0x1,  /* is_press:1 */
    QWS_KEY_FLAG_AUTO_REPEAT = 0x2,  /* is_auto_repeat:1 */
};

/* QWS_EVT_KEY — mirrors QWSKeyEvent::SimpleData wire layout:
 *   int window, uint keycode, Qt::KeyboardModifiers modifiers,
 *   ushort unicode, [2-byte pad], uint flags(is_press:1, is_auto_repeat:1) */
typedef struct {
    int32_t  window;
    uint32_t keycode;    /* Qt::Key value */
    uint32_t modifiers;  /* qws_keyboard_modifier */
    uint16_t unicode;    /* Unicode codepoint (ushort in Qt) */
    uint16_t flags;      /* qws_key_flag bits */
} qws_evt_key_t;
 
/* QWS_EVT_FOCUS */
typedef struct {
    int32_t window;
    int32_t get_focus;  /* 1=gained, 0=lost */
} qws_evt_focus_t;
 
/* QWS_EVT_REGION
 * simpleData is this struct; rawData is an array of qws_rect_t
 * containing nrectangles entries. */
typedef struct {
    int32_t window;
    int32_t nrectangles;
#ifdef QWS_CLIENTBLIT
    int32_t id;             /* client-blit id */
#endif
    uint8_t type;           /* 0=allocation, 1=direct-paint */
} qws_evt_region_t;
 
/* QWS_EVT_CREATION: server assigns a contiguous range of IDs.
 * The client receives IDs objectid through objectid+count-1. */
typedef struct {
    int32_t object_id;  /* first ID in the allocated range */
    int32_t count;      /* number of consecutive IDs allocated */
} qws_evt_creation_t;

/* QWS_EVT_MAX_WINDOW_RECT */
typedef struct {
    int32_t window;
    qws_rect_t rect;
} qws_evt_max_window_rect_t;
 
/* QWS_EVT_WINDOW_OPERATION */
typedef struct {
    int32_t window;
    int32_t operation;  /* cyclic enum: Show, Hide, ShowMaximized, etc. */
} qws_evt_window_operation_t;
 
/* QWS_EVT_PROPERTY_NOTIFY */
typedef struct {
    int32_t window;
    int32_t property;
    int32_t state;      /* 0=changed, 1=deleted */
} qws_evt_property_notify_t;
 
/* QWS_EVT_PROPERTY_REPLY: simpleData + rawData = property value bytes */
typedef struct {
    int32_t window;
    int32_t property;
    int32_t len;
} qws_evt_property_reply_t;
 
/* QWS_EVT_EMBED */
typedef struct {
    int32_t window;
    int32_t type;       /* cyclic enum values for embed ops */
} qws_evt_embed_t;

typedef enum {
    FONT_REMOVED = 0
} qws_font_event_t;
 
typedef struct {
    int32_t type;   /* only a single event defined - see above */
} qws_evt_font_t;

/* -----------------------------------------------------------
 * Command payloads (client → server simpleData structs)
 * ----------------------------------------------------------- */
 
/* QWS_CMD_IDENTIFY: sent by client immediately after connecting.
 * simpleData: { idLock, idLen }
 *   idLock = client's QWSLock semaphore ID (SysV semid or POSIX id)
 *   idLen  = byte length of the app name string in rawData
 * rawData: app name as UTF-16LE encoded string */
typedef struct {
    int32_t id_len;         /* byte length of app name in rawData */
    int32_t id_lock;        /* client's QWSLock ID for server to attach to */
} qws_cmd_identify_t;
 
/* QWS_CMD_CREATE */
typedef struct {
    int32_t count;      /* number of IDs to create */
} qws_cmd_create_t;
 
/* QWS_CMD_REGION_REQUEST */
typedef struct {
    int32_t window;
    int32_t surfacekeylength;
    int32_t surfacedatalength;
    int32_t nrectangles;
} qws_cmd_region_request_t;

typedef struct {
    int32_t mem_id;
    int32_t width;
    int32_t height;
    int32_t lock_id;
    int32_t format;
    int32_t flags;
} qws_cmd_region_surface_data_shm_t;

typedef union {
    uint8_t *raw;
    qws_cmd_region_surface_data_shm_t shm;
} qws_cmd_region_surface_data_t;
 
/* QWS_CMD_REGION_MOVE */
typedef struct {
    int32_t window;
    int32_t dx;
    int32_t dy;
} qws_cmd_region_move_t;
 
/* QWS_CMD_REGION_DESTROY */
typedef struct {
    int32_t window;
} qws_cmd_region_destroy_t;
 
/* QWS_CMD_REGION_NAME
 * rawData = UTF-16 encoded window name string */
typedef struct {
    int32_t window;
    int32_t name_len;       /* byte length of name in rawData */
    int32_t caption_len;    /* byte length of caption in rawData */
} qws_cmd_region_name_t;
 
/* QWSChangeAltitudeCommand::Altitude (Qt 4.8) */
enum qws_altitude {
    QWS_ALTITUDE_LOWER       = -1,
    QWS_ALTITUDE_RAISE       =  0,
    QWS_ALTITUDE_STAYS_ON_TOP =  1,
};

/* QWS_CMD_CHANGE_ALTITUDE */
typedef struct {
    int32_t window;
    int32_t altitude;   /* qws_altitude */
    int32_t is_fixed;
} qws_cmd_change_altitude_t;
 
/* QWS_CMD_REQUEST_FOCUS */
typedef struct {
    int32_t window;
    int32_t flag;       /* 0=set, 1=clear (?) */
} qws_cmd_request_focus_t;
 
/* QWS_CMD_SET_OPACITY */
typedef struct {
    int32_t window;
    int32_t opacity;    /* 0-255 */
} qws_cmd_set_opacity_t;
 
/* QWS_CMD_ADD_PROPERTY */
typedef struct {
    int32_t window;
    int32_t property;
} qws_cmd_add_property_t;
 
/* QWS_CMD_SET_PROPERTY
 * rawData = property value bytes */
typedef struct {
    int32_t window;
    int32_t property;
    int32_t mode;       /* 0=Replace, 1=Append, 2=Prepend */
} qws_cmd_set_property_t;
 
/* QWS_CMD_REMOVE_PROPERTY */
typedef struct {
    int32_t window;
    int32_t property;
} qws_cmd_remove_property_t;
 
/* QWS_CMD_GET_PROPERTY */
typedef struct {
    int32_t window;
    int32_t property;
} qws_cmd_get_property_t;
 
/* QWS_CMD_GRAB_MOUSE */
typedef struct {
    int32_t window;
    int32_t grab;       /* 1=grab, 0=ungrab */
} qws_cmd_grab_mouse_t;
 
/* QWS_CMD_GRAB_KEYBOARD */
typedef struct {
    int32_t window;
    int32_t grab;
} qws_cmd_grab_keyboard_t;
 
/* QWS_CMD_DEFINE_CURSOR
 * rawData = cursor image data */
typedef struct {
    int32_t window;
    int32_t width;
    int32_t height;
    int32_t hot_x;
    int32_t hot_y;
    int32_t id;
} qws_cmd_define_cursor_t;
 
/* QWS_CMD_SELECT_CURSOR */
typedef struct {
    int32_t window;
    int32_t cursor_id;
} qws_cmd_select_cursor_t;
 
/* QWS_CMD_REPAINT_REGION
 * rawData = array of qws_rect_t */
typedef struct {
    int32_t window;
    int32_t window_flags;
    int32_t opaque;
    int32_t nrectangles;
} qws_cmd_repaint_region_t;
 
/* QWS_CMD_SHUTDOWN: carries no simpleData and no rawData.
 * The client sends this to request a clean server-side teardown of its
 * session. The server closes the connection upon receipt. */
typedef struct {
    /* intentionally empty — QWSShutdownCommand has no payload fields */
} qws_cmd_shutdown_t;

/* QWS_CMD_QCOP_REGISTER: rawData = channel name (UTF-16LE) */
typedef struct {
    int32_t dummy;          /* unused, present for framing */
} qws_cmd_qcop_register_t;
 
/* QWS_CMD_QCOP_SEND: rawData = channel + message + data */
typedef struct {
    int32_t channel_len;
    int32_t message_len;
    int32_t data_len;
} qws_cmd_qcop_send_t;

/* QWSInputMethod::UpdateType (Qt 4.8) — carried in qws_cmd_im_update_t::type */
enum qws_im_update_type {
    QWS_IM_FOCUS_IN   = 0,  /* widget gained IM focus */
    QWS_IM_FOCUS_OUT  = 1,  /* widget lost IM focus */
    QWS_IM_UPDATE     = 2,  /* input context changed (cursor moved, etc.) */
    QWS_IM_DICTATION  = 3,  /* request dictation-mode input */
};

/* QWS_CMD_IM_UPDATE */
typedef struct {
    int32_t window;
    int32_t type;       /* qws_im_update_type */
    int32_t widget_id;
} qws_cmd_im_update_t;

/* QWS_CMD_IM_RESPONSE */
typedef struct {
    int32_t window;
    int32_t type;
} qws_cmd_im_response_t;
 
/* QWS_CMD_IM_MOUSE */
typedef struct {
    int32_t window;
    int32_t index;
    int32_t state;      /* IMMouse enum value */
} qws_cmd_im_mouse_t;
 
typedef enum {
        STARTED_USING_FONT = 0,
        STOPPED_USING_FONT,
} qws_font_cmd_t;

/* QWS_CMD_FONT */
typedef struct {
    int32_t type;       /* StartedUsing=0, StoppedUsing=1 */
} qws_cmd_font_t;
 
/* -----------------------------------------------------------
 * Generic packet representation
 * ----------------------------------------------------------- */
 
typedef struct {
    qws_packet_header_t header;
    void  *simple_data;     /* points to type-specific struct above */
    void  *raw_data;        /* variable-length payload, or NULL */
} qws_packet_t;
 
/* -----------------------------------------------------------
 * Type → simpleData size lookup
 *
 * Separate functions because events and commands both start at 0
 * (they flow in opposite directions on the wire).
 * ----------------------------------------------------------- */
 
/* Returns the fixed simpleData size for a given event type.
 * Returns 0 for types with no simpleData, -1 for unknown types. */
int32_t qws_event_simple_len(int32_t type);
 
/* Returns the fixed simpleData size for a given command type. */
int32_t qws_command_simple_len(int32_t type);
 
/* -----------------------------------------------------------
 * Allocate / free packets
 * ----------------------------------------------------------- */
 
qws_packet_t *qws_packet_alloc(int32_t type, size_t simple_len,
                                 size_t raw_len);
void           qws_packet_free(qws_packet_t *pkt);
 
/* -----------------------------------------------------------
 * Serialization / deserialization
 * ----------------------------------------------------------- */

/* Serialize a packet into a contiguous buffer for writing to socket.
 * Returns total byte count written to `buf`. Caller ensures buf is
 * large enough (use qws_packet_wire_size). */
size_t qws_packet_serialize(const qws_packet_t *pkt, void *buf, size_t buflen);

/* Total wire size of a packet */
size_t qws_packet_wire_size(const qws_packet_t *pkt);

/* -----------------------------------------------------------
 * Stream reader: incrementally parse packets from a byte stream
 * ----------------------------------------------------------- */
/* -----------------------------------------------------------
 * Stream reader: incrementally parse packets from a byte stream
 * ----------------------------------------------------------- */
 
typedef enum {
    QWS_READ_HEADER,
    QWS_READ_SIMPLE,
    QWS_READ_RAW,
    QWS_READ_DONE,
} qws_read_state_t;
 
typedef struct {
    qws_read_state_t  state;
    qws_packet_header_t hdr;
    uint8_t           *simple_buf;
    uint8_t           *raw_buf;
    size_t             bytes_read;  /* within current state */
    bool               reading_commands;  /* true=commands, false=events */
} qws_reader_t;
 
/* Initialize a reader.
 * reading_commands: true if reading client→server commands,
 *                   false if reading server→client events. */
void qws_reader_init(qws_reader_t *r, bool reading_commands);
 
/* Feed bytes into the reader. Returns number of bytes consumed.
 * When a complete packet is ready, `*out` is set (caller must
 * free with qws_packet_free). If no complete packet yet, *out = NULL. */
size_t qws_reader_feed(qws_reader_t *r, const void *data, size_t len,
                        qws_packet_t **out);
 
/* Reset reader state (e.g., on error) */
void qws_reader_reset(qws_reader_t *r);
 
/* -----------------------------------------------------------
 * Socket transport helpers
 * ----------------------------------------------------------- */
 
/* Create and bind the QWS server socket.
 * socket_path: e.g. "/tmp/qtembedded-<user>/QtEmbedded-0"
 * If socket_path is NULL, auto-generates from user + display=0.
 * Returns fd on success, -1 on error (errno set). */
int qws_server_listen(const char *socket_path);
 
/* Auto-generate the default QWS socket path for given display number.
 * Writes into buf (must be >= pathlen). Returns 0 on success. */
int qws_socket_path(int display, char *buf, size_t buflen);
 
/* Accept a QWS client connection. Returns client fd or -1. */
int qws_server_accept(int server_fd);
 
/* Convenience: write a complete packet to fd. Returns 0 on success. */
int qws_write_packet(int fd, const qws_packet_t *pkt);

/* -----------------------------------------------------------
 * Shared memory helpers
 * ----------------------------------------------------------- */

typedef enum {
    QWS_IPC_SYSV,       /* SysV IPC (semget/semop/semctl) */
    QWS_IPC_POSIX,       /* POSIX named semaphores (sem_open) */
} qws_ipc_type_t;

typedef struct {
    int    shm_id;     /* SysV shm id, or -1 if using mmap */
    int    fd;         /* mmap fd, or -1 if using SysV */
    void  *base;       /* mapped address */
    size_t size;       /* mapped size */
} qws_shm_t;

/* Create a new shared memory region of given size.
 * Returns 0 on success. */
int qws_shm_create(qws_shm_t *shm, size_t size, qws_ipc_type_t ipc_type);

/* Detach/destroy shared memory (also removes SysV segment). */
void qws_shm_destroy(qws_shm_t *shm);

/* Attach an existing SysV shm segment read-only (does NOT take ownership).
 * Returns 0 on success. */
int qws_shm_attach_sysv(qws_shm_t *shm, int shm_id);

/* Detach a previously attached mapping without deleting the segment. */
void qws_shm_detach(qws_shm_t *shm);

/* -----------------------------------------------------------
 * Utility: type name strings for debugging
 * ----------------------------------------------------------- */

const char *qws_event_type_name(int type);
const char *qws_command_type_name(int type);
const char *qws_image_format_name(int format);
const char *qws_surface_flag_name(int flag);
const char *qws_im_update_type_name(int type);
const char *qws_altitude_name(int altitude);
const char *qws_window_type_str(uint32_t flags);
bool qws_is_synchronous_commmand(int type);


#ifdef __cplusplus
}
#endif

#endif /* QWS_PROTO_H */
