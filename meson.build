project('qwswayland', 'c', 'cpp',
  version: '0.1.0',
  meson_version: '1.7.0',
  default_options: {
    'warning_level': '2',
  })

add_project_arguments(
  '-Wformat=2',
  '-Werror=format',
  '-Werror=format-security',
  '-Werror=return-type',
  '-Werror=shadow',
  language: ['c', 'cpp'],
)

add_project_arguments(
  '-Werror=int-conversion',
  '-Werror=incompatible-pointer-types',
  language: ['c'],
)

add_project_arguments(
  '-Werror=implicit-function-declaration',
  '-Werror=strict-prototypes',
  language: 'c',
)

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