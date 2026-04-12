qt6 = import('qt6')

qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick', 'WaylandCompositor'])
qt6_core_dep = dependency('qt6', modules: ['Core'])

moc_files = qt6.compile_moc(
  headers: ['surfacethumbnail.h'],
  dependencies: qt6_dep,
)

# Unlike CMake's qt_add_executable(), Meson's compile_resources() does not
# auto-detect whether Qt was built with zstd support. If it wasn't, rcc still
# tries to use zstd compression and blows up at link time. So we have to
# replicate what Qt's own CMake scripts do internally: query qt_config and
# pass --no-zstd ourselves. Thanks for nothing, build system ecosystem.
_qt6_config = qt6_core_dep.get_variable(pkgconfig: 'qt_config', default_value: '')
_rcc_args = _qt6_config.contains('zstd') ? [] : ['--no-zstd']

rcc_files = qt6.compile_resources(sources: 'qtshell.qrc', extra_args: _rcc_args)

executable('qtshell',
  'main.cpp',
  moc_files,
  rcc_files,
  dependencies: qt6_dep,
  cpp_args: ['-DQT_QML_DEBUG'],
  override_options: {'cpp_std': 'c++17'},
  install: true,
)
