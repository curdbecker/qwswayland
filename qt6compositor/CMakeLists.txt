# Fallback only for the Qt Creator under MacOS
# Unfortunately, the Meson support is quite rudimentary, so it
# is easier to keep a separate CMake configuration around.
cmake_minimum_required(VERSION 3.16)
project(qtshell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick WaylandCompositor)

qt_add_executable(qtshell
    MACOSX_BUNDLE
    main.cpp
    surfacethumbnail.h
)

target_link_libraries(qtshell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::WaylandCompositor
)

target_sources(qtshell PRIVATE qtshell.qrc)
