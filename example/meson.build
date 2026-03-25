# pkg_config_path is set per-subproject via the parent's meson.options
# (qt4_pkg_config_path), so dependency('qt4') finds Qt Embedded here
# without affecting the rest of the build.
qt4_mod = import('qt4')
qt4_dep = dependency('qt4', modules: ['Core', 'Gui'])

preprocessed = qt4_mod.preprocess(
  moc_headers: ['mainwindow.h'],
  qresources: ['example.qrc'],
  dependencies: qt4_dep,
)

executable('example',
  sources: ['main.cpp', 'mainwindow.cpp'] + preprocessed,
  dependencies: qt4_dep,
)
