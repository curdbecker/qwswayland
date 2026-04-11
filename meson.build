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

# assert() is used to enforce internal invariants that must catch violations
# even in release builds (e.g. detecting protocol state corruption early).
# -UNDEBUG unconditionally undefines NDEBUG so assertions are always active.
add_project_arguments('-UNDEBUG', language: ['c', 'cpp'])

asan_override = get_option('asan') ? ['b_sanitize=address'] : []

subdir('libqwsproto')

screen_interposer_deps = []
if get_option('build_screen_interposers')
  subdir('screen_interposers/linuxfb')
  screen_interposer_deps += linuxfb_headers_dep
endif

if get_option('build_proxy')
  subdir('proxy')
endif

if get_option('build_trace_proxy')
  subdir('trace_proxy')
endif

if get_option('build_example') and not meson.is_cross_build()
  subdir('example')
endif