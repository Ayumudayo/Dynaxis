add_library(server_core
    src/net/acceptor.cpp
    src/net/session.cpp
    src/net/dispatcher.cpp
    src/util/log.cpp
)

target_include_directories(server_core PUBLIC include)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    # 헤더온리 boost::system 사용: 라이브러리 링크 생략, 매크로 정의 필요
    target_compile_definitions(server_core PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    target_include_directories(server_core PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_link_libraries(server_core PUBLIC Boost::system)
endif()

if (WIN32)
    # Asio의 WinSock 종속성
    target_link_libraries(server_core PUBLIC ws2_32 mswsock)
endif()

if (MSVC)
    target_compile_definitions(server_core PUBLIC _WIN32_WINNT=0x0602)
endif()

# opcode 헤더 생성에 의존 (가능한 경우)
if (TARGET generate_opcodes)
    add_dependencies(server_core generate_opcodes)
endif()
if (TARGET generate_wire_codec)
    add_dependencies(server_core generate_wire_codec)
endif()

# 설치 규칙: 다른 프로그램이 코어의 패킷 구조를 공유할 수 있도록 export
install(TARGETS server_core
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.hpp")
