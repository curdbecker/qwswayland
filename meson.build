project('qwswayland', 'c', 'cpp',
  version: '0.1.0',
  meson_version: '1.7.0',
  default_options: {
    'warning_level': '3',
  })

add_project_arguments(
  '-Werror',
  language: ['c', 'cpp'],
)
subdir('libqwsproto')

if get_option('build_proxy')
  subdir('proxy')
endif

if get_option('build_trace_proxy')
  subdir('trace_proxy')
endif

if get_option('build_example') and not meson.is_cross_build()
  subdir('example')
endif