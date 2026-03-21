cmake_minimum_required(VERSION 3.16)


add_library(qwsproto STATIC
    qws_proto.c
    qws_server_helpers.c
    qws_trace.c
    qws_lock.c
)

target_include_directories(qwsproto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Need -lrt for shm_open, -lpthread for sem_open on some systems
target_link_libraries(qwsproto PUBLIC rt pthread)

# Install headers
install(FILES qws_proto.h qws_server_helpers.h qws_trace.h qws_lock.h
        DESTINATION include/qwsproto)
install(TARGETS qwsproto ARCHIVE DESTINATION lib)