cmake_minimum_required(VERSION 3.20)

find_package(Protobuf REQUIRED)

set(WIRE_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/wire.proto)

protobuf_generate_cpp(WIRE_SRCS WIRE_HDRS ${WIRE_PROTO})

add_library(wire_proto STATIC ${WIRE_SRCS} ${WIRE_HDRS})
target_include_directories(wire_proto PUBLIC ${Protobuf_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wire_proto PUBLIC ${Protobuf_LIBRARIES})

