cmake_minimum_required(VERSION 3.20)

find_package(Protobuf REQUIRED)
include(GNUInstallDirs)


set(WIRE_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/wire.proto)

protobuf_generate_cpp(WIRE_SRCS WIRE_HDRS ${WIRE_PROTO})

add_library(wire_proto STATIC ${WIRE_SRCS} ${WIRE_HDRS})
target_include_directories(wire_proto PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(wire_proto PUBLIC protobuf::libprotobuf)

install(TARGETS wire_proto
        EXPORT server_coreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ${WIRE_HDRS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
