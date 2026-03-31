cc = meson.get_compiler('c')

if get_option('ipc_backend') == 'posix'
  add_project_arguments('-DQWS_IPC_POSIX', language: 'c')
endif

qwsproto_sources = files(
  'qws_lock.c',
  'qws_pcap.c',
  'qws_proto.c',
  'qws_qdatastream.c',
  'qws_rect.c',
  'qws_event_factory.c',
  'qws_unicode.c',
  'qws_trace.c',
)

libpcap_dep = dependency('libpcap', required: false)
pcap_args = libpcap_dep.found() ? ['-DQWS_HAVE_PCAP'] : []

qwsproto_deps = [
  dependency('threads'),
  libpcap_dep,
  cc.find_library('rt', required: false),
]

qwsproto_lib = static_library('qwsproto',
  qwsproto_sources,
  dependencies: qwsproto_deps,
  c_args: pcap_args,
  install: true,
)

qwsproto_dep = declare_dependency(
  link_with: qwsproto_lib,
  include_directories: include_directories('.'),
  dependencies: qwsproto_deps,
  compile_args: pcap_args,
)

install_headers(
  'qws_event_factory.h', 'qws_lock.h', 'qws_pcap.h', 'qws_proto.h', 'qws_qdatastream.h', 'qws_rect.h', 'qws_trace.h', 'qws_unicode.h',
  subdir: 'qwsproto',
)
