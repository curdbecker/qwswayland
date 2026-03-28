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

xdg_output_src = wayland_mod.scan_xml(
  wp_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  client: true, server: false)

alpha_modifier_src = wayland_mod.scan_xml(
  wp_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml',
  client: true, server: false)

proxy_sources = files(
  'main.c',
  'client.c',
  'debug.c',
  'lifecycle.c',
  'proxy.c',
  'seat.c',
  'window.c',
)

executable('qwswayland',
  proxy_sources,
  xdg_shell_src,
  xdg_output_src,
  alpha_modifier_src,
  dependencies: [
    qwsproto_dep,
    wayland_client,
    xkbcommon,
    icu_uc,
    stc,
  ],
  install: true,
)
