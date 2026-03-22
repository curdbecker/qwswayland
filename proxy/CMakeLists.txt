cmake_minimum_required(VERSION 3.16)

pkg_check_modules(XKB REQUIRED xkbcommon)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ICU_UC REQUIRED icu-uc)

# Generate xdg-shell protocol bindings — required
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)


pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(WAYLAND_PROTOCOL_SPECS
    "stable/xdg-shell/xdg-shell.xml"
    "unstable/xdg-output/xdg-output-unstable-v1.xml"
)

set(GENERATED_PROTOCOL_SOURCES "")

foreach(PROTO_REL_XML IN LISTS WAYLAND_PROTOCOL_SPECS)
    get_filename_component(PROTO_NAME "${PROTO_REL_XML}" NAME_WLE)

    set(PROTO_XML "${WL_PROTOCOLS_DIR}/${PROTO_REL_XML}")
    if(NOT EXISTS "${PROTO_XML}")
        message(FATAL_ERROR "${PROTO_NAME}: protocol XML not found at ${PROTO_XML}")
    endif()

    set(PROTO_CLIENT_H "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-client-protocol.h")
    set(PROTO_CODE_C   "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}-protocol.c")

    add_custom_command(
        OUTPUT "${PROTO_CLIENT_H}"
        COMMAND ${WAYLAND_SCANNER} client-header "${PROTO_XML}" "${PROTO_CLIENT_H}"
        DEPENDS "${PROTO_XML}"
        COMMENT "Generating ${PROTO_NAME} client header"
    )
    add_custom_command(
        OUTPUT "${PROTO_CODE_C}"
        COMMAND ${WAYLAND_SCANNER} private-code "${PROTO_XML}" "${PROTO_CODE_C}"
        DEPENDS "${PROTO_XML}" "${PROTO_CLIENT_H}"
        COMMENT "Generating ${PROTO_NAME} private code"
    )

    list(APPEND GENERATED_PROTOCOL_SOURCES "${PROTO_CODE_C}")
    message(STATUS "Will generate ${PROTO_NAME} protocol bindings from ${PROTO_XML}")
endforeach()

add_executable(qwswayland
    main.c
    connection.c
    lifecycle.c
    proxy.c
    seat.c
    window.c
    ${GENERATED_PROTOCOL_SOURCES}
)

target_include_directories(qwswayland PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}  # for generated protocol headers
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${ICU_UC_INCLUDE_DIRS}
)

target_link_libraries(qwswayland PRIVATE
    qwsproto
    ${WAYLAND_CLIENT_LIBRARIES}
    ${XKB_LIBRARIES}
    ${ICU_UC_LIBRARIES}
)

install(TARGETS qwswayland RUNTIME DESTINATION bin)
