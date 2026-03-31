trace_proxy_sources = files(
  'main.c',
  'trace_proxy.c',
)

executable('qws_trace_proxy',
  trace_proxy_sources,
  dependencies: [
    qwsproto_dep,
    dependency('threads'),
  ],
  install: true,
)
