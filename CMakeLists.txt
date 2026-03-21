cmake_minimum_required(VERSION 3.16)
project(qwswayland VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(BUILD_PROXY "Build the QWSWayland proxy daemon" ON)

# libqwsproto - always built, no external deps beyond POSIX
add_subdirectory(libqwsproto)

# Proxy daemon requires Wayland
if(BUILD_PROXY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_CURSOR wayland-cursor)
    pkg_check_modules(XKB xkbcommon)
    add_subdirectory(proxy)
endif()
