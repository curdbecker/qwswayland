project('qwswayland', 'c', 'cpp',
  version: '0.1.0',
  meson_version: '1.7.0',
  default_options: {
    'warning_level': '1',
  })

subdir('libqwsproto')

if get_option('build_proxy')
  subdir('proxy')
endif

if get_option('build_trace_proxy')
  subdir('trace_proxy')
endif

if get_option('build_example')
  subdir('example')
endif