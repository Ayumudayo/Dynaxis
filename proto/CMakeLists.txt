cmake_minimum_required(VERSION 3.20)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(WIRE_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/wire.proto)
set(GATEWAY_LB_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/gateway_lb.proto)

protobuf_generate_cpp(WIRE_SRCS WIRE_HDRS ${WIRE_PROTO})

set(GATEWAY_LB_PROTO_SRCS)
set(GATEWAY_LB_PROTO_HDRS)
protobuf_generate_cpp(GATEWAY_LB_PROTO_SRCS GATEWAY_LB_PROTO_HDRS ${GATEWAY_LB_PROTO})

set(GATEWAY_LB_GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/gateway_lb.grpc.pb.cc")
set(GATEWAY_LB_GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/gateway_lb.grpc.pb.h")

add_custom_command(
    OUTPUT ${GATEWAY_LB_GRPC_SRCS} ${GATEWAY_LB_GRPC_HDRS}
    COMMAND $<TARGET_FILE:protobuf::protoc>
    ARGS --grpc_out ${CMAKE_CURRENT_BINARY_DIR}
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         -I ${CMAKE_CURRENT_SOURCE_DIR}
         ${GATEWAY_LB_PROTO}
    DEPENDS ${GATEWAY_LB_PROTO} protobuf::protoc gRPC::grpc_cpp_plugin
)

add_library(wire_proto STATIC ${WIRE_SRCS} ${WIRE_HDRS})
target_include_directories(wire_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wire_proto PUBLIC protobuf::libprotobuf)

add_library(gateway_lb_proto STATIC
    ${GATEWAY_LB_PROTO_SRCS}
    ${GATEWAY_LB_PROTO_HDRS}
    ${GATEWAY_LB_GRPC_SRCS}
    ${GATEWAY_LB_GRPC_HDRS}
)
target_include_directories(gateway_lb_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gateway_lb_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
