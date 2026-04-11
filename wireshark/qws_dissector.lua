--[[
 qws_dissector.lua — Wireshark Lua dissector for QWS (Qt Window System) protocol

 Capture file format: DLT_USER0 (link type 147)
 Each frame = one complete QWS message, produced by:
   qws_trace_proxy -w <file.pcapng>

 Install:
   cp wireshark/qws_dissector.lua ~/.config/wireshark/plugins/
   Reload Wireshark (Ctrl-Shift-L) or restart.

 Frame layout (written by libqwsproto/qws_pcap.c):
   Offset  Size  Field
      0      1   direction  (0=client→server / command, 1=server→client / event)
      1      1   client_id  (session number, starts at 1)
      2      2   reserved   (zeroed)
      4      4   type       (int32 LE — QWS_CMD_* or QWS_EVT_*)
      8      4   raw_len    (int32 LE — rawData byte count)
     12    var   simpleData (fixed per-type struct)
   12+s    var   rawData    (raw_len bytes)

 Address convention (enables ip.addr filters and the Conversations dialog):
   Server:   10.0.0.0
   Client N: 10.0.0.N   (N = client_id from capture header)
--]]

local qws_proto = Proto("qws", "Qt Window System Protocol")

-- Expert info: colors the packet row red in the packet list
local ef_dropped = ProtoExpert.new("qws.expert.dropped", "Packet dropped by proxy",
                                    expert.group.SECURITY, expert.severity.ERROR)
qws_proto.experts = { ef_dropped }

-- -----------------------------------------------------------------------
-- Image format name table (QImage::Format, Qt 4.8)
-- -----------------------------------------------------------------------

local IMAGE_FORMAT_NAMES = {
    [0]  = "Invalid",       [1]  = "Mono",            [2]  = "MonoLSB",
    [3]  = "Indexed8",      [4]  = "RGB32",            [5]  = "ARGB32",
    [6]  = "ARGB32_Pre",    [7]  = "RGB16",            [8]  = "ARGB8565_Pre",
    [9]  = "RGB666",        [10] = "ARGB6666_Pre",     [11] = "RGB555",
    [12] = "ARGB8555_Pre",  [13] = "RGB888",           [14] = "RGB444",
    [15] = "ARGB4444_Pre",
}

-- Qt::WindowType values (bits 0-7 of window_flags)
local WINDOW_TYPE_NAMES = {
    [0x00] = "Widget",      [0x01] = "Window",      [0x03] = "Dialog",
    [0x05] = "Sheet",       [0x07] = "Drawer",      [0x09] = "Popup",
    [0x0b] = "Tool",        [0x0d] = "ToolTip",     [0x0f] = "SplashScreen",
    [0x11] = "Desktop",     [0x12] = "SubWindow",
}

-- -----------------------------------------------------------------------
-- Field declarations
-- -----------------------------------------------------------------------

local f = qws_proto.fields

-- Capture header
f.direction   = ProtoField.uint8 ("qws.direction",   "Direction",  base.DEC,
                  {[0]="Command (C→S)", [1]="Event (S→C)"})
f.client_id   = ProtoField.uint8 ("qws.client_id",   "Client ID",  base.DEC)
f.reserved    = ProtoField.uint16("qws.reserved",    "Reserved",   base.HEX)
f.dropped     = ProtoField.bool  ("qws.dropped",     "Dropped",    16, nil, 0x0001)

-- QWS wire header
f.msg_type    = ProtoField.int32 ("qws.type",        "Type",       base.DEC)
f.raw_len     = ProtoField.int32 ("qws.raw_len",     "Raw Length", base.DEC)

-- Generic fallback blobs
f.simple_data = ProtoField.bytes ("qws.simple_data", "Simple Data")
f.raw_data    = ProtoField.bytes ("qws.raw_data",    "Raw Data")

-- Shared per-type integer fields
f.window        = ProtoField.int32 ("qws.window",       "Window ID",       base.DEC)
f.count         = ProtoField.int32 ("qws.count",        "Count",           base.DEC)
f.nrects        = ProtoField.int32 ("qws.nrects",       "Num Rectangles",  base.DEC)
f.property      = ProtoField.int32 ("qws.property",     "Property ID",     base.DEC)
f.grab          = ProtoField.int32 ("qws.grab",         "Grab",            base.DEC,
                    {[0]="Release", [1]="Grab"})
f.operation     = ProtoField.int32 ("qws.operation",    "Operation",       base.DEC)

-- Rectangle fields (reused per-rect in every decoded rect array)
f.rect_x1       = ProtoField.int32("qws.rect.x1",       "x1",              base.DEC)
f.rect_y1       = ProtoField.int32("qws.rect.y1",       "y1",              base.DEC)
f.rect_x2       = ProtoField.int32("qws.rect.x2",       "x2",              base.DEC)
f.rect_y2       = ProtoField.int32("qws.rect.y2",       "y2",              base.DEC)

-- String rawData fields
f.app_name       = ProtoField.string("qws.app_name",       "App Name")
f.display_spec   = ProtoField.string("qws.display_spec",   "Display Spec")
f.window_name    = ProtoField.string("qws.window_name",    "Window Name")
f.window_caption = ProtoField.string("qws.window_caption", "Window Caption")
f.surface_key    = ProtoField.string("qws.surface_key",    "Surface Key")
f.font_name      = ProtoField.string("qws.font_name",      "Font Name")
f.qcop_channel   = ProtoField.string("qws.qcop_channel",   "QCop Channel")
f.qcop_message   = ProtoField.string("qws.qcop_message",   "QCop Message")

-- Surface shm data fields
f.shm_mem_id    = ProtoField.int32 ("qws.shm.mem_id",   "SHM mem_id",    base.DEC)
f.shm_width     = ProtoField.int32 ("qws.shm.width",    "Width",         base.DEC)
f.shm_height    = ProtoField.int32 ("qws.shm.height",   "Height",        base.DEC)
f.shm_lock_id   = ProtoField.int32 ("qws.shm.lock_id",  "Lock ID",       base.DEC)
f.shm_format    = ProtoField.int32 ("qws.shm.format",   "Format",        base.DEC,
                    IMAGE_FORMAT_NAMES)
-- Surface flags bitmask (QWSWindowSurface::SurfaceFlag)
f.shm_flags          = ProtoField.uint32("qws.shm.flags",          "Surface Flags",   base.HEX)
f.shm_flag_reserved  = ProtoField.bool  ("qws.shm.flag.reserved",  "RegionReserved",  32, nil, 0x01)
f.shm_flag_buffered  = ProtoField.bool  ("qws.shm.flag.buffered",  "Buffered",        32, nil, 0x02)
f.shm_flag_opaque    = ProtoField.bool  ("qws.shm.flag.opaque",    "Opaque",          32, nil, 0x04)

-- Connected event
f.conn_len      = ProtoField.int32 ("qws.conn_len",     "Display Spec Len", base.DEC)
f.conn_clientid = ProtoField.int32 ("qws.conn.clientid","Client ID",        base.DEC)
f.shm_id        = ProtoField.int32 ("qws.shm_id",       "SHM ID",           base.DEC)

-- Creation event
f.object_id     = ProtoField.int32 ("qws.object_id",    "First Object ID",  base.DEC)

-- Region event
f.region_type   = ProtoField.uint8 ("qws.region_type",  "Region Type",      base.DEC,
                    {[0]="Allocation", [1]="DirectPaint"})

-- Mouse event
f.x_root        = ProtoField.int32 ("qws.x_root",       "X (root)",         base.DEC)
f.y_root        = ProtoField.int32 ("qws.y_root",       "Y (root)",         base.DEC)
f.mouse_state   = ProtoField.uint32("qws.state",        "Button/Modifier State", base.HEX)
f.delta         = ProtoField.int32 ("qws.delta",        "Wheel Delta",      base.DEC)
f.time_ms       = ProtoField.uint32("qws.time_ms",      "Timestamp (ms)",   base.DEC)

-- Key event
f.keycode       = ProtoField.uint32("qws.keycode",      "Qt::Key",          base.HEX)
f.modifiers     = ProtoField.uint32("qws.modifiers",    "Modifiers",        base.HEX)
f.unicode       = ProtoField.uint16("qws.unicode",      "Unicode",          base.HEX)
f.key_flags     = ProtoField.uint16("qws.key_flags",    "Flags",            base.HEX)

-- Focus event / RequestFocus command
f.focus_flag    = ProtoField.int32 ("qws.focus_flag",   "Focus",            base.DEC,
                    {[0]="Lost", [1]="Gained"})

-- Identify command
f.id_len        = ProtoField.int32 ("qws.id_len",       "App Name Len (bytes)", base.DEC)
f.id_lock       = ProtoField.int32 ("qws.id_lock",      "Lock ID",          base.DEC)

-- RegionName command
f.name_len      = ProtoField.int32 ("qws.name_len",     "Name Len (bytes)", base.DEC)
f.caption_len   = ProtoField.int32 ("qws.caption_len",  "Caption Len (bytes)", base.DEC)

-- Window flags (Qt::WindowFlags) — RepaintRegion command
-- Low byte (0xff) = Qt::WindowType; upper bytes = hint bits
f.window_flags       = ProtoField.uint32("qws.window_flags",      "Window Flags",              base.HEX)
f.wf_type            = ProtoField.uint32("qws.wf.type",           "Window Type",               base.DEC, WINDOW_TYPE_NAMES, 0xff)
f.wf_mswin_fixed     = ProtoField.bool  ("qws.wf.mswin_fixed",    "MSWindowsFixedSizeDialog",  32, nil, 0x00000100)
f.wf_mswin_own_dc    = ProtoField.bool  ("qws.wf.mswin_own_dc",   "MSWindowsOwnDC",            32, nil, 0x00000200)
f.wf_x11_bypass_wm   = ProtoField.bool  ("qws.wf.x11_bypass_wm", "X11BypassWindowManager",    32, nil, 0x00000400)
f.wf_frameless       = ProtoField.bool  ("qws.wf.frameless",      "Frameless",                 32, nil, 0x00000800)
f.wf_title           = ProtoField.bool  ("qws.wf.title",          "WindowTitle",               32, nil, 0x00001000)
f.wf_system_menu     = ProtoField.bool  ("qws.wf.system_menu",    "WindowSystemMenu",          32, nil, 0x00002000)
f.wf_minimize        = ProtoField.bool  ("qws.wf.minimize",       "WindowMinimizeButton",      32, nil, 0x00004000)
f.wf_maximize        = ProtoField.bool  ("qws.wf.maximize",       "WindowMaximizeButton",      32, nil, 0x00008000)
f.wf_ctx_help        = ProtoField.bool  ("qws.wf.ctx_help",       "WindowContextHelpButton",   32, nil, 0x00010000)
f.wf_shade           = ProtoField.bool  ("qws.wf.shade",          "WindowShadeButton",         32, nil, 0x00020000)
f.wf_stays_on_top    = ProtoField.bool  ("qws.wf.stays_on_top",   "WindowStaysOnTop",          32, nil, 0x00040000)
f.wf_ok_button       = ProtoField.bool  ("qws.wf.ok_button",      "WindowOkButton",            32, nil, 0x00080000)
f.wf_cancel_button   = ProtoField.bool  ("qws.wf.cancel_button",  "WindowCancelButton",        32, nil, 0x00100000)
f.wf_customize       = ProtoField.bool  ("qws.wf.customize",      "CustomizeWindow",           32, nil, 0x02000000)
f.wf_stays_on_bottom = ProtoField.bool  ("qws.wf.stays_on_bottom","WindowStaysOnBottom",       32, nil, 0x04000000)
f.wf_close_button    = ProtoField.bool  ("qws.wf.close_button",   "WindowCloseButton",         32, nil, 0x08000000)
f.wf_mac_toolbar     = ProtoField.bool  ("qws.wf.mac_toolbar",    "MacWindowToolBarButton",    32, nil, 0x10000000)
f.wf_bypass_proxy    = ProtoField.bool  ("qws.wf.bypass_proxy",   "BypassGraphicsProxy",       32, nil, 0x20000000)
f.wf_softkeys_vis    = ProtoField.bool  ("qws.wf.softkeys_vis",   "WindowSoftkeysVisible",     32, nil, 0x40000000)
f.wf_softkeys_resp   = ProtoField.bool  ("qws.wf.softkeys_resp",  "WindowSoftkeysRespond",     32, nil, 0x80000000)
f.opaque             = ProtoField.int32 ("qws.opaque",            "Opaque",                    base.DEC)

-- ChangeAltitude command
f.altitude      = ProtoField.int32 ("qws.altitude",     "Altitude",         base.DEC,
                    {[-1]="Lower", [0]="Raise", [1]="StaysOnTop"})
f.is_fixed      = ProtoField.int32 ("qws.is_fixed",     "Is Fixed",         base.DEC)

-- SetOpacity command
f.opacity       = ProtoField.uint8 ("qws.opacity",      "Opacity (0-255)",  base.DEC)

-- Region command
f.surf_key_len  = ProtoField.int32 ("qws.surf_key_len", "Surface Key Len",  base.DEC)
f.surf_data_len = ProtoField.int32 ("qws.surf_data_len","Surface Data Len", base.DEC)

-- RegionMove command
f.dx            = ProtoField.int32 ("qws.dx",           "dX",               base.DEC)
f.dy            = ProtoField.int32 ("qws.dy",           "dY",               base.DEC)

-- SetProperty command
f.prop_mode     = ProtoField.int32 ("qws.prop_mode",    "Mode",             base.DEC,
                    {[0]="Replace", [1]="Append", [2]="Prepend"})

-- PropertyNotify event
f.prop_state    = ProtoField.int32 ("qws.prop_state",   "State",            base.DEC,
                    {[0]="Changed", [1]="Deleted"})

-- PropertyReply event
f.prop_len      = ProtoField.int32 ("qws.prop_len",     "Value Length",     base.DEC)

-- DefineCursor command
f.cur_width     = ProtoField.int32 ("qws.cur_width",    "Width",            base.DEC)
f.cur_height    = ProtoField.int32 ("qws.cur_height",   "Height",           base.DEC)
f.cur_hot_x     = ProtoField.int32 ("qws.cur_hot_x",    "Hot X",            base.DEC)
f.cur_hot_y     = ProtoField.int32 ("qws.cur_hot_y",    "Hot Y",            base.DEC)
f.cur_id        = ProtoField.int32 ("qws.cur_id",       "Cursor ID",        base.DEC)
f.cursor_id     = ProtoField.int32 ("qws.cursor_id",    "Cursor ID",        base.DEC)

-- QCopSend command
f.channel_len   = ProtoField.int32 ("qws.channel_len",  "Channel Len",      base.DEC)
f.message_len   = ProtoField.int32 ("qws.message_len",  "Message Len",      base.DEC)
f.data_len      = ProtoField.int32 ("qws.data_len",     "Data Len",         base.DEC)

-- IMUpdate command
f.im_type       = ProtoField.int32 ("qws.im_type",      "IM Update Type",   base.DEC,
                    {[0]="FocusIn", [1]="FocusOut", [2]="Update", [3]="Dictation"})
f.widget_id     = ProtoField.int32 ("qws.widget_id",    "Widget ID",        base.DEC)

-- SelectionClear / SelectionRequest / SelectionNotify events
f.requestor      = ProtoField.int32("qws.requestor",     "Requestor Window",  base.DEC)
f.mimetypes      = ProtoField.int32("qws.mimetypes",     "MIME Types Prop",   base.DEC)
f.mimetype       = ProtoField.int32("qws.mimetype",      "MIME Type Prop",    base.DEC)

-- QCopMessage event
f.is_response    = ProtoField.int32("qws.is_response",   "Is Response",       base.DEC,
                     {[0]="Request", [1]="Response"})
f.lchannel       = ProtoField.int32("qws.lchannel",      "Channel Len (chars)",base.DEC)
f.lmessage       = ProtoField.int32("qws.lmessage",      "Message Len (chars)",base.DEC)
f.ldata          = ProtoField.int32("qws.ldata",         "Data Len (bytes)",  base.DEC)

-- IMEvent / IMInit events
f.replace_from   = ProtoField.int32("qws.replace_from",  "Replace From",      base.DEC)
f.replace_length = ProtoField.int32("qws.replace_length","Replace Length",    base.DEC)
f.existence      = ProtoField.int32("qws.existence",     "IM Exists",         base.DEC,
                     {[0]="Destroyed", [1]="Created"})

-- ScreenTransform event + command
f.screen         = ProtoField.int32("qws.screen",        "Screen Index",      base.DEC)
f.transformation = ProtoField.int32("qws.transformation","Transformation",    base.DEC)

-- SetSelectionOwner command
f.sel_windowid   = ProtoField.int32("qws.sel_windowid",  "Window ID",         base.DEC)
f.hour           = ProtoField.int32("qws.hour",          "Hour",              base.DEC)
f.minute         = ProtoField.int32("qws.minute",        "Minute",            base.DEC)
f.sec            = ProtoField.int32("qws.sec",           "Second",            base.DEC)
f.ms             = ProtoField.int32("qws.ms",            "Millisecond",       base.DEC)

-- ConvertSelection command
f.sel_selection  = ProtoField.int32("qws.sel_selection", "Selection Prop",    base.DEC)

-- PositionCursor command
f.new_x          = ProtoField.int32("qws.new_x",         "New X",             base.DEC)
f.new_y          = ProtoField.int32("qws.new_y",         "New Y",             base.DEC)

-- PlaySound command
f.sound_filename = ProtoField.string("qws.sound_filename","Filename")

-- Embed command
f.embedder       = ProtoField.int32("qws.embedder",      "Embedder Window",   base.DEC)
f.embedded       = ProtoField.int32("qws.embedded",      "Embedded Window",   base.DEC)
f.embed_type     = ProtoField.int32("qws.embed_type",    "Embed Type",        base.DEC,
                     {[1]="Start", [2]="Stop", [4]="Region"})

-- Font event
f.font_type      = ProtoField.int32("qws.font_type",     "Font Event Type",   base.DEC,
                     {[0]="FontRemoved"})

-- -----------------------------------------------------------------------
-- Type name tables — mirror qws_proto.h enums
-- -----------------------------------------------------------------------

local CMD_NAMES = {
    [0]  = "Unknown",        [1]  = "Create",          [2]  = "Shutdown",
    [3]  = "Region",         [4]  = "RegionMove",      [5]  = "RegionDestroy",
    [6]  = "SetProperty",    [7]  = "AddProperty",     [8]  = "RemoveProperty",
    [9]  = "GetProperty",    [10] = "SetSelectionOwner",[11] = "ConvertSelection",
    [12] = "RequestFocus",   [13] = "ChangeAltitude",  [14] = "SetOpacity",
    [15] = "DefineCursor",   [16] = "SelectCursor",    [17] = "PositionCursor",
    [18] = "GrabMouse",      [19] = "PlaySound",       [20] = "QCopRegister",
    [21] = "QCopSend",       [22] = "RegionName",      [23] = "Identify",
    [24] = "GrabKeyboard",   [25] = "RepaintRegion",   [26] = "IMMouse",
    [27] = "IMUpdate",       [28] = "IMResponse",      [29] = "Embed",
    [30] = "Font",           [31] = "ScreenTransform",
}

local EVT_NAMES = {
    [0]  = "NoEvent",        [1]  = "Connected",       [2]  = "Mouse",
    [3]  = "Focus",          [4]  = "Key",             [5]  = "Region",
    [6]  = "Creation",       [7]  = "PropertyNotify",  [8]  = "PropertyReply",
    [9]  = "SelectionClear", [10] = "SelectionRequest",[11] = "SelectionNotify",
    [12] = "MaxWindowRect",  [13] = "QCopMessage",     [14] = "WindowOperation",
    [15] = "IMEvent",        [16] = "IMQuery",         [17] = "IMInit",
    [18] = "Embed",          [19] = "Font",            [20] = "ScreenTransform",
}

-- -----------------------------------------------------------------------
-- Simple data byte lengths — mirrors sizeof() of each qws_*_t struct.
-- Indexed as SIMPLE_LEN[direction][type_id].
-- direction: 0=command (C→S), 1=event (S→C)
-- -----------------------------------------------------------------------

local SIMPLE_LEN = {
    [0] = {  -- commands (qws_cmd_*_t)
        [0]  = 0,   -- Unknown
        [1]  = 4,   -- Create            {count:i32}
        [2]  = 0,   -- Shutdown          (empty struct)
        [3]  = 16,  -- Region            {window,surfacekeylength,surfacedatalength,nrectangles}
        [4]  = 12,  -- RegionMove        {window,dx,dy}
        [5]  = 4,   -- RegionDestroy     {window}
        [6]  = 12,  -- SetProperty       {window,property,mode}
        [7]  = 8,   -- AddProperty       {window,property}
        [8]  = 8,   -- RemoveProperty    {window,property}
        [9]  = 8,   -- GetProperty       {window,property}
        [10] = 20,  -- SetSelectionOwner  {windowid,hour,minute,sec,ms}
        [11] = 12,  -- ConvertSelection   {requestor,selection,mimetypes}
        [12] = 8,   -- RequestFocus      {window,flag}
        [13] = 12,  -- ChangeAltitude    {window,altitude,is_fixed}
        [14] = 8,   -- SetOpacity        {window,opacity}
        [15] = 20,  -- DefineCursor      {width,height,hot_x,hot_y,id}
        [16] = 8,   -- SelectCursor      {window,cursor_id}
        [17] = 8,   -- PositionCursor     {new_x,new_y}
        [18] = 8,   -- GrabMouse         {window,grab}
        [19] = 4,   -- PlaySound         {windowid}
        [20] = 4,   -- QCopRegister      {dummy}
        [21] = 12,  -- QCopSend          {channel_len,message_len,data_len}
        [22] = 12,  -- RegionName        {window,name_len,caption_len}
        [23] = 8,   -- Identify          {id_len,id_lock}
        [24] = 8,   -- GrabKeyboard      {window,grab}
        [25] = 16,  -- RepaintRegion     {window,window_flags,opaque,nrectangles}
        [26] = 12,  -- IMMouse           {window,index,state}
        [27] = 12,  -- IMUpdate          {window,type,widget_id}
        [28] = 8,   -- IMResponse        {window,type}
        [29] = 16,  -- Embed             {embedder,embedded,type,nrects}
        [30] = 4,   -- Font              {type}
        [31] = 8,   -- ScreenTransform   {screen,transformation}
    },
    [1] = {  -- events (qws_evt_*_t)
        [0]  = 0,   -- NoEvent
        [1]  = 16,  -- Connected         {window,len,client_id,server_shm_id}
        [2]  = 24,  -- Mouse             {window,x_root,y_root,state,delta,time}
        [3]  = 8,   -- Focus             {window,get_focus}
        [4]  = 16,  -- Key               {window,keycode,modifiers,unicode,flags}
        [5]  = 12,  -- Region            {window,nrectangles,type} (padded to 12)
        [6]  = 8,   -- Creation          {object_id,count}
        [7]  = 12,  -- PropertyNotify    {window,property,state}
        [8]  = 12,  -- PropertyReply     {window,property,len}
        [9]  = 4,   -- SelectionClear     {window}
        [10] = 16,  -- SelectionRequest   {window,requestor,property,mimetypes}
        [11] = 16,  -- SelectionNotify    {window,requestor,property,mimetype}
        [12] = 20,  -- MaxWindowRect     {window,rect{x1,y1,x2,y2}}
        [13] = 16,  -- QCopMessage        {is_response,lchannel,lmessage,ldata}
        [14] = 8,   -- WindowOperation   {window,operation}
        [15] = 12,  -- IMEvent            {window,replace_from,replace_length}
        [16] = 8,   -- IMQuery            {window,property}
        [17] = 8,   -- IMInit             {window,existence}
        [18] = 12,  -- Embed             {window,nrectangles,type}
        [19] = 4,   -- Font               {type}
        [20] = 8,   -- ScreenTransform    {screen,transformation}
    },
}

-- -----------------------------------------------------------------------
-- UTF-16LE helpers
-- -----------------------------------------------------------------------

-- Extract ASCII characters from a UTF-16LE tvb range.
-- QWS strings (app names, surface keys, window titles) are 7-bit ASCII
-- stored as UTF-16LE, so this covers all practical cases.
local function utf16le_tostring(tvb_range)
    local result = ""
    local n = tvb_range:len()
    for i = 0, n - 2, 2 do
        local b = tvb_range:bytes():get_index(i)
        if b == 0 then break end  -- null terminator
        result = result .. string.char((b >= 32 and b < 127) and b or 0x3f)
    end
    return result
end

-- Add a string field to `parent`, overriding its display value with the
-- decoded UTF-16LE text so Wireshark shows the readable string.
local function add_utf16le(parent, field, tvb_range)
    if tvb_range:len() == 0 then return end
    parent:add(field, tvb_range, utf16le_tostring(tvb_range))
end

-- -----------------------------------------------------------------------
-- Rectangle array helper
-- -----------------------------------------------------------------------

local RECT_SIZE = 16  -- 4 × int32

local function decode_rects(parent, tvb, offset, count, label)
    if count <= 0 then return end
    local total = count * RECT_SIZE
    local list  = parent:add(qws_proto, tvb(offset, total),
                              string.format("%s (%d)", label or "Rectangles", count))
    for i = 0, count - 1 do
        local o  = offset + i * RECT_SIZE
        local x1 = tvb(o,      4):le_int()
        local y1 = tvb(o +  4, 4):le_int()
        local x2 = tvb(o +  8, 4):le_int()
        local y2 = tvb(o + 12, 4):le_int()
        local r  = list:add(qws_proto, tvb(o, RECT_SIZE),
                             string.format("rect[%d]: (%d,%d)–(%d,%d)",
                                           i, x1, y1, x2, y2))
        r:add_le(f.rect_x1, tvb(o,      4))
        r:add_le(f.rect_y1, tvb(o +  4, 4))
        r:add_le(f.rect_x2, tvb(o +  8, 4))
        r:add_le(f.rect_y2, tvb(o + 12, 4))
    end
end

-- -----------------------------------------------------------------------
-- Per-type simpleData field decoder
-- -----------------------------------------------------------------------

local function decode_simple(subtree, tvb, offset, direction, type_id)
    local o = offset
    if direction == 0 then
        -- Commands
        if type_id == 23 then           -- Identify
            subtree:add_le(f.id_len,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.id_lock, tvb(o, 4))
        elseif type_id == 1 then        -- Create
            subtree:add_le(f.count, tvb(o, 4))
        elseif type_id == 3 then        -- Region
            subtree:add_le(f.window,        tvb(o, 4)); o = o + 4
            subtree:add_le(f.surf_key_len,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.surf_data_len, tvb(o, 4)); o = o + 4
            subtree:add_le(f.nrects,        tvb(o, 4))
        elseif type_id == 4 then        -- RegionMove
            subtree:add_le(f.window, tvb(o, 4)); o = o + 4
            subtree:add_le(f.dx,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.dy,     tvb(o, 4))
        elseif type_id == 5 then        -- RegionDestroy
            subtree:add_le(f.window, tvb(o, 4))
        elseif type_id == 22 then       -- RegionName
            subtree:add_le(f.window,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.name_len,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.caption_len, tvb(o, 4))
        elseif type_id == 25 then       -- RepaintRegion
            subtree:add_le(f.window, tvb(o, 4)); o = o + 4
            local wf = subtree:add_le(f.window_flags, tvb(o, 4))
            wf:add_le(f.wf_type,            tvb(o, 4))
            wf:add_le(f.wf_mswin_fixed,     tvb(o, 4))
            wf:add_le(f.wf_mswin_own_dc,    tvb(o, 4))
            wf:add_le(f.wf_x11_bypass_wm,   tvb(o, 4))
            wf:add_le(f.wf_frameless,       tvb(o, 4))
            wf:add_le(f.wf_title,           tvb(o, 4))
            wf:add_le(f.wf_system_menu,     tvb(o, 4))
            wf:add_le(f.wf_minimize,        tvb(o, 4))
            wf:add_le(f.wf_maximize,        tvb(o, 4))
            wf:add_le(f.wf_ctx_help,        tvb(o, 4))
            wf:add_le(f.wf_shade,           tvb(o, 4))
            wf:add_le(f.wf_stays_on_top,    tvb(o, 4))
            wf:add_le(f.wf_ok_button,       tvb(o, 4))
            wf:add_le(f.wf_cancel_button,   tvb(o, 4))
            wf:add_le(f.wf_customize,       tvb(o, 4))
            wf:add_le(f.wf_stays_on_bottom, tvb(o, 4))
            wf:add_le(f.wf_close_button,    tvb(o, 4))
            wf:add_le(f.wf_mac_toolbar,     tvb(o, 4))
            wf:add_le(f.wf_bypass_proxy,    tvb(o, 4))
            wf:add_le(f.wf_softkeys_vis,    tvb(o, 4))
            wf:add_le(f.wf_softkeys_resp,   tvb(o, 4))
            o = o + 4
            subtree:add_le(f.opaque,  tvb(o, 1)); o = o + 4
            subtree:add_le(f.nrects,  tvb(o, 4))
        elseif type_id == 13 then       -- ChangeAltitude
            subtree:add_le(f.window,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.altitude, tvb(o, 4)); o = o + 4
            subtree:add_le(f.is_fixed, tvb(o, 4))
        elseif type_id == 14 then       -- SetOpacity
            subtree:add_le(f.window,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.opacity, tvb(o, 1))
        elseif type_id == 12 then       -- RequestFocus
            subtree:add_le(f.window,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.focus_flag, tvb(o, 4))
        elseif type_id == 18 then       -- GrabMouse
            subtree:add_le(f.window, tvb(o, 4)); o = o + 4
            subtree:add_le(f.grab,   tvb(o, 4))
        elseif type_id == 24 then       -- GrabKeyboard
            subtree:add_le(f.window, tvb(o, 4)); o = o + 4
            subtree:add_le(f.grab,   tvb(o, 4))
        elseif type_id == 6 then        -- SetProperty
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.property,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.prop_mode, tvb(o, 4))
        elseif type_id == 7 or type_id == 8 or type_id == 9 then  -- Add/Remove/GetProperty
            subtree:add_le(f.window,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.property, tvb(o, 4))
        elseif type_id == 15 then       -- DefineCursor
            subtree:add_le(f.cur_width,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.cur_height, tvb(o, 4)); o = o + 4
            subtree:add_le(f.cur_hot_x,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.cur_hot_y,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.cur_id,     tvb(o, 4))
        elseif type_id == 16 then       -- SelectCursor
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.cursor_id, tvb(o, 4))
        elseif type_id == 21 then       -- QCopSend
            subtree:add_le(f.channel_len, tvb(o, 4)); o = o + 4
            subtree:add_le(f.message_len, tvb(o, 4)); o = o + 4
            subtree:add_le(f.data_len,    tvb(o, 4))
        elseif type_id == 27 then       -- IMUpdate
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.im_type,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.widget_id, tvb(o, 4))
        elseif type_id == 10 then       -- SetSelectionOwner
            subtree:add_le(f.sel_windowid, tvb(o, 4)); o = o + 4
            subtree:add_le(f.hour,         tvb(o, 4)); o = o + 4
            subtree:add_le(f.minute,       tvb(o, 4)); o = o + 4
            subtree:add_le(f.sec,          tvb(o, 4)); o = o + 4
            subtree:add_le(f.ms,           tvb(o, 4))
        elseif type_id == 11 then       -- ConvertSelection
            subtree:add_le(f.requestor,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.sel_selection,tvb(o, 4)); o = o + 4
            subtree:add_le(f.mimetypes,    tvb(o, 4))
        elseif type_id == 17 then       -- PositionCursor
            subtree:add_le(f.new_x, tvb(o, 4)); o = o + 4
            subtree:add_le(f.new_y, tvb(o, 4))
        elseif type_id == 19 then       -- PlaySound
            subtree:add_le(f.sel_windowid, tvb(o, 4))
        elseif type_id == 29 then       -- Embed
            subtree:add_le(f.embedder,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.embedded,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.embed_type, tvb(o, 4)); o = o + 4
            subtree:add_le(f.nrects,     tvb(o, 4))
        elseif type_id == 31 then       -- ScreenTransform
            subtree:add_le(f.screen,         tvb(o, 4)); o = o + 4
            subtree:add_le(f.transformation, tvb(o, 4))
        end
    else
        -- Events
        if type_id == 1 then            -- Connected
            subtree:add_le(f.window,        tvb(o, 4)); o = o + 4
            subtree:add_le(f.conn_len,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.conn_clientid, tvb(o, 4)); o = o + 4
            subtree:add_le(f.shm_id,        tvb(o, 4))
        elseif type_id == 6 then        -- Creation
            subtree:add_le(f.object_id, tvb(o, 4)); o = o + 4
            subtree:add_le(f.count,     tvb(o, 4))
        elseif type_id == 5 then        -- Region
            subtree:add_le(f.window,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.nrects,      tvb(o, 4)); o = o + 4
            subtree:add   (f.region_type, tvb(o, 1))
        elseif type_id == 2 then        -- Mouse
            subtree:add_le(f.window,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.x_root,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.y_root,      tvb(o, 4)); o = o + 4
            subtree:add_le(f.mouse_state, tvb(o, 4)); o = o + 4
            subtree:add_le(f.delta,       tvb(o, 4)); o = o + 4
            subtree:add_le(f.time_ms,     tvb(o, 4))
        elseif type_id == 4 then        -- Key
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.keycode,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.modifiers, tvb(o, 4)); o = o + 4
            subtree:add_le(f.unicode,   tvb(o, 2)); o = o + 2
            subtree:add_le(f.key_flags, tvb(o, 2))
        elseif type_id == 3 then        -- Focus
            subtree:add_le(f.window,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.focus_flag, tvb(o, 4))
        elseif type_id == 7 then        -- PropertyNotify
            subtree:add_le(f.window,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.property,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.prop_state, tvb(o, 4))
        elseif type_id == 8 then        -- PropertyReply
            subtree:add_le(f.window,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.property, tvb(o, 4)); o = o + 4
            subtree:add_le(f.prop_len, tvb(o, 4))
        elseif type_id == 14 then       -- WindowOperation
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.operation, tvb(o, 4))
        elseif type_id == 18 then       -- Embed
            subtree:add_le(f.window,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.nrects,     tvb(o, 4)); o = o + 4
            subtree:add_le(f.embed_type, tvb(o, 4))
        elseif type_id == 9 then        -- SelectionClear
            subtree:add_le(f.window, tvb(o, 4))
        elseif type_id == 10 then       -- SelectionRequest
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.requestor, tvb(o, 4)); o = o + 4
            subtree:add_le(f.property,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.mimetypes, tvb(o, 4))
        elseif type_id == 11 then       -- SelectionNotify
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.requestor, tvb(o, 4)); o = o + 4
            subtree:add_le(f.property,  tvb(o, 4)); o = o + 4
            subtree:add_le(f.mimetype,  tvb(o, 4))
        elseif type_id == 13 then       -- QCopMessage
            subtree:add_le(f.is_response, tvb(o, 4)); o = o + 4
            subtree:add_le(f.lchannel,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.lmessage,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.ldata,       tvb(o, 4))
        elseif type_id == 15 then       -- IMEvent
            subtree:add_le(f.window,         tvb(o, 4)); o = o + 4
            subtree:add_le(f.replace_from,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.replace_length, tvb(o, 4))
        elseif type_id == 16 then       -- IMQuery
            subtree:add_le(f.window,   tvb(o, 4)); o = o + 4
            subtree:add_le(f.property, tvb(o, 4))
        elseif type_id == 17 then       -- IMInit
            subtree:add_le(f.window,    tvb(o, 4)); o = o + 4
            subtree:add_le(f.existence, tvb(o, 4))
        elseif type_id == 19 then       -- Font
            subtree:add_le(f.font_type, tvb(o, 4))
        elseif type_id == 20 then       -- ScreenTransform
            subtree:add_le(f.screen,         tvb(o, 4)); o = o + 4
            subtree:add_le(f.transformation, tvb(o, 4))
        end
    end
end

-- -----------------------------------------------------------------------
-- Per-type rawData decoder
-- Mirrors qws_trace_decode_command / qws_trace_decode_event from qws_trace.c.
-- simpleData starts at tvb offset 12; values are read from there as needed.
-- -----------------------------------------------------------------------

local function decode_raw(root, tvb, raw_offset, raw_len, direction, type_id)
    if raw_len <= 0 then return end
    local o  = raw_offset
    local sd = 12   -- simpleData base offset in tvb

    if direction == 0 then
        -- Commands

        if type_id == 23 then
            -- Identify: rawData = UTF-16LE app name
            add_utf16le(root, f.app_name, tvb(o, raw_len))

        elseif type_id == 22 then
            -- RegionName: name (name_len bytes) + caption (caption_len bytes)
            local name_bytes    = tvb(sd + 4, 4):le_int()
            local caption_bytes = tvb(sd + 8, 4):le_int()
            if name_bytes > 0 and raw_len >= name_bytes then
                add_utf16le(root, f.window_name, tvb(o, name_bytes))
            end
            if caption_bytes > 0 and raw_len >= name_bytes + caption_bytes then
                add_utf16le(root, f.window_caption, tvb(o + name_bytes, caption_bytes))
            end

        elseif type_id == 3 then
            -- Region: rects[] + surface key (UTF-16LE) + surface data
            local nrects      = tvb(sd + 12, 4):le_int()
            local key_chars   = tvb(sd +  4, 4):le_int()
            local data_len    = tvb(sd +  8, 4):le_int()
            local rects_bytes = nrects * RECT_SIZE
            local key_bytes   = key_chars * 2
            if nrects > 0 and raw_len >= rects_bytes then
                decode_rects(root, tvb, o, nrects, "Rectangles")
            end
            local key_off  = o + rects_bytes
            local data_off = key_off + key_bytes
            if key_chars > 0 and raw_len >= rects_bytes + key_bytes then
                local key_str = utf16le_tostring(tvb(key_off, key_bytes))
                add_utf16le(root, f.surface_key, tvb(key_off, key_bytes))
                local remaining = raw_len - rects_bytes - key_bytes
                if key_str == "shm" and data_len >= 24 and remaining >= 24 then
                    local st = root:add(qws_proto, tvb(data_off, 24), "Surface Data (shm)")
                    st:add_le(f.shm_mem_id,  tvb(data_off,      4))
                    st:add_le(f.shm_width,   tvb(data_off +  4, 4))
                    st:add_le(f.shm_height,  tvb(data_off +  8, 4))
                    st:add_le(f.shm_lock_id, tvb(data_off + 12, 4))
                    st:add_le(f.shm_format,  tvb(data_off + 16, 4))
                    local sf = st:add_le(f.shm_flags, tvb(data_off + 20, 4))
                    sf:add_le(f.shm_flag_reserved, tvb(data_off + 20, 4))
                    sf:add_le(f.shm_flag_buffered, tvb(data_off + 20, 4))
                    sf:add_le(f.shm_flag_opaque,   tvb(data_off + 20, 4))
                elseif data_len > 0 and remaining > 0 then
                    root:add(f.raw_data, tvb(data_off, math.min(data_len, remaining)))
                end
            end

        elseif type_id == 25 then
            -- RepaintRegion: rects[]
            local nrects = tvb(sd + 12, 4):le_int()
            if nrects > 0 and raw_len >= nrects * RECT_SIZE then
                decode_rects(root, tvb, o, nrects, "Rectangles")
            end

        elseif type_id == 30 then
            -- Font: ASCII font name
            root:add(f.font_name, tvb(o, raw_len))

        elseif type_id == 20 then
            -- QCopRegister: UTF-16LE channel name
            add_utf16le(root, f.qcop_channel, tvb(o, raw_len))

        elseif type_id == 21 then
            -- QCopSend: channel (ch_len UTF-16LE) + message (msg_len UTF-16LE) + data
            local ch_bytes  = tvb(sd,     4):le_int()
            local msg_bytes = tvb(sd + 4, 4):le_int()
            local dat_len   = tvb(sd + 8, 4):le_int()
            if ch_bytes  > 0 and raw_len >= ch_bytes then
                add_utf16le(root, f.qcop_channel, tvb(o, ch_bytes))
            end
            if msg_bytes > 0 and raw_len >= ch_bytes + msg_bytes then
                add_utf16le(root, f.qcop_message, tvb(o + ch_bytes, msg_bytes))
            end
            local dat_off = ch_bytes + msg_bytes
            if dat_len > 0 and raw_len >= dat_off + dat_len then
                root:add(f.raw_data, tvb(o + dat_off, dat_len))
            end

        elseif type_id == 19 then
            -- PlaySound: rawData = UTF-16LE filename
            add_utf16le(root, f.sound_filename, tvb(o, raw_len))

        elseif type_id == 29 then
            -- Embed: rawData = nrects × QRect (x,y,w,h as int32 LE each)
            local nrects = tvb(sd + 12, 4):le_int()
            if nrects > 0 and raw_len >= nrects * RECT_SIZE then
                decode_rects(root, tvb, o, nrects, "Embed Rectangles")
            end

        else
            root:add(f.raw_data, tvb(o, raw_len))  -- generic fallback
        end

    else
        -- Events

        if type_id == 1 then
            -- Connected: ASCII display spec (length from simpleData.len field)
            local display_len = tvb(sd + 4, 4):le_int()
            if display_len > 0 and raw_len >= display_len then
                root:add(f.display_spec, tvb(o, display_len))
            end

        elseif type_id == 5 then
            -- Region: rects[]
            local nrects = tvb(sd + 4, 4):le_int()
            if nrects > 0 and raw_len >= nrects * RECT_SIZE then
                decode_rects(root, tvb, o, nrects, "Rectangles")
            end

        elseif type_id == 13 then
            -- QCopMessage: channel (UTF-16LE) + message (UTF-16LE) + opaque data
            local lchannel  = tvb(sd + 4,  4):le_int()
            local lmessage  = tvb(sd + 8,  4):le_int()
            local ldata     = tvb(sd + 12, 4):le_int()
            local ch_bytes  = lchannel * 2
            local msg_bytes = lmessage * 2
            if ch_bytes > 0 and raw_len >= ch_bytes then
                add_utf16le(root, f.qcop_channel, tvb(o, ch_bytes))
            end
            if msg_bytes > 0 and raw_len >= ch_bytes + msg_bytes then
                add_utf16le(root, f.qcop_message, tvb(o + ch_bytes, msg_bytes))
            end
            local dat_off = ch_bytes + msg_bytes
            if ldata > 0 and raw_len >= dat_off + ldata then
                root:add(f.raw_data, tvb(o + dat_off, ldata))
            end

        elseif type_id == 18 then
            -- Embed: rawData = nrects × QRect (x,y,w,h as int32 LE each)
            local nrects = tvb(sd + 4, 4):le_int()
            if nrects > 0 and raw_len >= nrects * RECT_SIZE then
                decode_rects(root, tvb, o, nrects, "Embed Rectangles")
            end

        elseif type_id == 15 or type_id == 17 then
            -- IMEvent / IMInit: opaque serialised IM state blob
            root:add(f.raw_data, tvb(o, raw_len))

        else
            root:add(f.raw_data, tvb(o, raw_len))  -- generic fallback
        end
    end
end

-- -----------------------------------------------------------------------
-- Main dissector function
-- -----------------------------------------------------------------------

function qws_proto.dissector(tvb, pinfo, tree)
    local pkt_len = tvb:len()
    if pkt_len < 12 then return 0 end  -- 4 capture header + 8 QWS header minimum

    pinfo.cols.protocol:set("QWS")

    -- Capture header
    local direction     = tvb(0, 1):uint()
    local client_id_val = tvb(1, 1):uint()

    -- Source / Destination — populates Wireshark's Source/Destination columns
    -- and enables standard ip.src/dst/addr display filters.
    --   Server:   10.0.0.0
    --   Client N: 10.0.0.N
    local client_addr = Address.ip("10.0.0." .. client_id_val)
    local server_addr = Address.ip("10.0.0.0")
    if direction == 0 then   -- C→S command
        pinfo.src = client_addr
        pinfo.dst = server_addr
    else                      -- S→C event
        pinfo.src = server_addr
        pinfo.dst = client_addr
    end

    -- QWS wire header (little-endian int32s)
    local type_id = tvb(4, 4):le_int()
    local raw_len = tvb(8, 4):le_int()

    -- Resolve type name and simpleData size
    local type_names = (direction == 0) and CMD_NAMES or EVT_NAMES
    local type_name  = type_names[type_id] or string.format("type=0x%x", type_id)
    local dir_arrow  = (direction == 0) and "CMD" or "EVT"
    local simple_len = ((SIMPLE_LEN[direction] or {})[type_id]) or 0

    pinfo.cols.info:set(string.format("[%s] %s", dir_arrow, type_name))

    -- Root tree item
    local root = tree:add(qws_proto, tvb(),
                           string.format("Qt Window System Protocol, %s %s",
                                         dir_arrow, type_name))

    -- Capture header subtree
    local cap_flags = tvb(2, 2):le_uint()
    local is_dropped = (cap_flags & 0x0001) ~= 0
    if is_dropped then
        pinfo.cols.info:prepend("[DROPPED] ")
        root:add_proto_expert_info(ef_dropped, "Packet dropped by proxy")
    end
    local cap_tree = root:add(qws_proto, tvb(0, 4), "Capture Header")
    cap_tree:add    (f.direction, tvb(0, 1))
    cap_tree:add    (f.client_id, tvb(1, 1))
    cap_tree:add_le (f.reserved,  tvb(2, 2))
    cap_tree:add_le (f.dropped,   tvb(2, 2))

    -- QWS wire header subtree
    local hdr_tree = root:add(qws_proto, tvb(4, 8), "QWS Header")
    hdr_tree:add_le(f.msg_type, tvb(4, 4)):append_text(
        string.format("  (%s)", type_name))
    hdr_tree:add_le(f.raw_len, tvb(8, 4))

    -- Simple data subtree
    if simple_len > 0 and pkt_len >= 12 + simple_len then
        local sd_tree = root:add(qws_proto, tvb(12, simple_len),
                                  string.format("Simple Data (%d bytes)", simple_len))
        decode_simple(sd_tree, tvb, 12, direction, type_id)
    end

    -- Raw data (decoded per-type, matching qws_trace.c field-level output)
    local raw_offset = 12 + simple_len
    if raw_len > 0 and pkt_len >= raw_offset + raw_len then
        decode_raw(root, tvb, raw_offset, raw_len, direction, type_id)
    end

    return pkt_len
end

-- -----------------------------------------------------------------------
-- Register for DLT_USER0 (wtap link type 147)
-- -----------------------------------------------------------------------

DissectorTable.get("wtap_encap"):add(wtap.USER0, qws_proto)
