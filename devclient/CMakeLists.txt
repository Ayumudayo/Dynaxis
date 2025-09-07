cmake_minimum_required(VERSION 3.20)

# FTXUI 기반 최소 클라이언트(처음부터 다시 시작)
add_executable(dev_chat_cli
    src/main.cpp
    src/net_client.cpp
    src/store.cpp
)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)

# 코어 프로토콜 헤더 사용
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(dev_chat_cli PRIVATE wire_proto)

# FTXUI 의존성 (vcpkg)
find_package(ftxui CONFIG REQUIRED)
target_link_libraries(dev_chat_cli PRIVATE ftxui::screen ftxui::dom ftxui::component)

# Boost.Asio 사용: header-only boost::system 모드 지원
if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    target_compile_definitions(dev_chat_cli PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    target_include_directories(dev_chat_cli PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_link_libraries(dev_chat_cli PRIVATE Boost::system)
endif()

if (WIN32)
    target_link_libraries(dev_chat_cli PRIVATE ws2_32 mswsock)
endif()

if (MSVC)
    target_compile_definitions(dev_chat_cli PUBLIC _WIN32_WINNT=0x0602)
    # Visual Studio에서 인스톨/구성 매핑 경고 회피
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
endif()
