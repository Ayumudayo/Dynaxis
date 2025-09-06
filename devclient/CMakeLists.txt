add_executable(dev_chat_cli src/main.cpp)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    target_compile_definitions(dev_chat_cli PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    target_include_directories(dev_chat_cli PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_link_libraries(dev_chat_cli PRIVATE Boost::system)
endif()

if (MSVC)
    target_compile_definitions(dev_chat_cli PUBLIC _WIN32_WINNT=0x0602)
endif()

if (WIN32)
    target_link_libraries(dev_chat_cli PRIVATE ws2_32 mswsock)
endif()
