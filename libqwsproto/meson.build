cc = meson.get_compiler('c')

if get_option('ipc_backend') == 'posix'
  add_project_arguments('-DQWS_IPC_POSIX', language: 'c')
endif

qwsproto_sources = files(
  'qws_lock.c',
  'qws_proto.c',
  'qws_server_helpers.c',
  'qws_trace.c',
)

qwsproto_deps = [
  dependency('threads'),
  cc.find_library('rt', required: false),
]

qwsproto_lib = static_library('qwsproto',
  qwsproto_sources,
  dependencies: qwsproto_deps,
  install: true,
)

qwsproto_dep = declare_dependency(
  link_with: qwsproto_lib,
  include_directories: include_directories('.'),
)

install_headers(
  'qws_lock.h', 'qws_proto.h', 'qws_server_helpers.h', 'qws_trace.h',
  subdir: 'qwsproto',
)
