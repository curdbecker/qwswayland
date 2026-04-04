wayland_mod = import('unstable-wayland')

wayland_client = dependency('wayland-client')
xkbcommon      = dependency('xkbcommon')
icu_uc         = dependency('icu-uc')
wayland_protos = dependency('wayland-protocols')
stc            = dependency('stc')   # from subprojects/stc.wrap

# Protocol XML locations
wp_dir = wayland_protos.get_variable('pkgdatadir')

xdg_shell_src = wayland_mod.scan_xml(
  wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  client: true, server: false)

alpha_modifier_src = wayland_mod.scan_xml(
  wp_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml',
  client: true, server: false)

cursor_shape_src = wayland_mod.scan_xml(
  wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  client: true, server: false)

# required by cursor-shape-v1
tablet_tool_src = wayland_mod.scan_xml(
  wp_dir / 'stable/tablet/tablet-v2.xml',
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
  c_args: ['-w'],
)

executable('qwswayland',
  proxy_sources,
  link_with: wayland_proto_lib,
  dependencies: [
    qwsproto_dep,
    wayland_client,
    xkbcommon,
    icu_uc,
    stc,
  ],
  install: true,
)
