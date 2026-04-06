wayland_mod = import('unstable-wayland')
fs = import('fs')

wayland_client = dependency('wayland-client')
xkbcommon      = dependency('xkbcommon')
wayland_protos = dependency('wayland-protocols')
stc            = dependency('stc')   # from subprojects/stc.wrap

# Protocol XML locations
wp_dir = wayland_protos.get_variable('pkgdatadir')

xdg_shell_src = wayland_mod.scan_xml(
  wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  client: true, server: false)

alpha_modifier_src = []
alpha_modifier_args = []
if fs.exists(wp_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml')
  alpha_modifier_src = wayland_mod.scan_xml(
    wp_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml',
    client: true, server: false)
  alpha_modifier_args = ['-DHAVE_ALPHA_MODIFIER_V1']
endif

cursor_shape_src = wayland_mod.scan_xml(
  wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  client: true, server: false)

# required by cursor-shape-v1; stable tablet-v2 superseded the unstable variant
_tablet_xml = wp_dir / 'stable/tablet/tablet-v2.xml'
if not fs.exists(_tablet_xml)
  _tablet_xml = wp_dir / 'unstable/tablet/tablet-unstable-v2.xml'
endif
tablet_tool_src = wayland_mod.scan_xml(_tablet_xml,
  client: true, server: false)

qt_shell_src = wayland_mod.scan_xml(
'protocols/qt-shell-unstable-v1.xml',
client: true, server: false)

proxy_sources = files(
  'cursor.c',
  'main.c',
  'client.c',
  'clipboard.c',
  'debug.c',
  'lifecycle.c',
  'property_store.c',
  'proxy.c',
  'seat.c',
  'window.c',
)

# Generated Wayland protocol glue — compiled silently, not our code
wayland_proto_lib = static_library('wayland_proto_generated',
  xdg_shell_src,
  alpha_modifier_src,
  qt_shell_src,
  cursor_shape_src,
  tablet_tool_src,
  dependencies: [
    wayland_client,
  ],
  c_args: ['-w'],
)

executable('qwswayland',
  proxy_sources,
  link_with: wayland_proto_lib,
  c_args: alpha_modifier_args,
  dependencies: [
    qwsproto_dep,
    wayland_client,
    xkbcommon,
    stc,
    screen_interposer_deps,
  ],
  install: true,
)
