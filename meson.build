project('qwswayland', 'c',
  version: '0.1.0',
  meson_version: '1.7.0',
  default_options: ['warning_level=1'])

subdir('libqwsproto')

if get_option('build_proxy')
  subdir('proxy')
endif