trace_proxy_sources = files(
  'main.c',
  'trace_proxy.c',
)

executable('qws_trace_proxy',
  trace_proxy_sources,
  override_options: asan_override,
  dependencies: [
    qwsproto_dep,
    dependency('threads'),
  ],
  install: true,
)
