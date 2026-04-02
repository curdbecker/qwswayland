qt4_mod = import('qt4')
qt4_dep = dependency('qt4', modules: ['Core', 'Gui'])

preprocessed = qt4_mod.preprocess(
  moc_headers: ['mainwindow.h'],
  qresources: ['example.qrc'],
  dependencies: qt4_dep,
)

executable('example',
  sources: ['main.cpp', 'mainwindow.cpp', 'debug.cpp'] + preprocessed,
  dependencies: qt4_dep,
  cpp_args: '-Wno-error'
)
