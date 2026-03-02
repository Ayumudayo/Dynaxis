add_executable(server_app
    src/main.cpp
    src/app/bootstrap.cpp
    src/app/config.cpp
    src/app/core_internal_adapter.cpp
    src/app/metrics_server.cpp
    src/app/router.cpp
    src/chat/chat_service_core.cpp
    src/chat/chat_hook_plugin_manager.cpp
    src/chat/chat_hook_plugin_chain.cpp
    src/chat/handlers_login.cpp
    src/chat/session_events.cpp
    src/chat/handlers_join.cpp
    src/chat/handlers_leave.cpp
    src/chat/handlers_chat.cpp
    src/chat/handlers_ping.cpp
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_core wire_proto server_storage_pg server_storage_redis server_state)
# dlopen/dlsym (Linux)
target_link_libraries(server_app PRIVATE ${CMAKE_DL_LIBS})
# protobuf 생성 헤더 경로 추가
target_include_directories(server_app PRIVATE ${CMAKE_BINARY_DIR}/proto)

# Game Opcodes 생성
if (Python3_Interpreter_FOUND)
    set(GAME_OPCODES_JSON ${CMAKE_SOURCE_DIR}/server/protocol/game_opcodes.json)
    set(GAME_GENERATED_HEADER ${CMAKE_SOURCE_DIR}/server/include/server/protocol/game_opcodes.hpp)
    add_custom_command(
        OUTPUT ${GAME_GENERATED_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py ${GAME_OPCODES_JSON} ${GAME_GENERATED_HEADER}
        DEPENDS ${GAME_OPCODES_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py
        COMMENT "Generating protocol game opcodes header"
        VERBATIM)
    add_custom_target(generate_game_opcodes DEPENDS ${GAME_GENERATED_HEADER})
    add_dependencies(server_app generate_game_opcodes)
endif()

# Runtime DLL 자동 복사(Windows)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -Druntime_dlls=$<TARGET_RUNTIME_DLLS:server_app>
        -Dtarget_dir=$<TARGET_FILE_DIR:server_app>
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls_if_any.cmake
        COMMAND_EXPAND_LISTS)
endif()

# Postgres storage adapter
add_library(server_storage_pg STATIC
    src/storage/postgres/connection_pool.cpp
)
target_include_directories(server_storage_pg PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_pg PUBLIC server_core)

# libpqxx 필수: provider가 libpqxx::pqxx 또는 unofficial::libpqxx::libpqxx를 제공해야 함
find_package(libpqxx CONFIG QUIET)
if (libpqxx_FOUND AND TARGET libpqxx::pqxx)
    target_link_libraries(server_storage_pg PUBLIC libpqxx::pqxx)
else()
    pkg_check_modules(libpqxx QUIET IMPORTED_TARGET libpqxx)
    if (libpqxx_FOUND)
        target_link_libraries(server_storage_pg PUBLIC PkgConfig::libpqxx)
    else()
        find_package(unofficial-libpqxx CONFIG QUIET)
        if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
            target_link_libraries(server_storage_pg PUBLIC unofficial::libpqxx::libpqxx)
        else()
            message(FATAL_ERROR "libpqxx not found — PostgreSQL storage requires libpqxx")
        endif()
    endif()
endif()

# Redis client/pool (skeleton)
add_library(server_storage_redis STATIC
    src/storage/redis/client.cpp
)
target_include_directories(server_storage_redis PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_redis PUBLIC server_core)

# Try to find redis-plus-plus exported targets
find_package(redis++ CONFIG QUIET)
if (TARGET redis++::redis++)
    target_link_libraries(server_storage_redis PUBLIC redis++::redis++)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++)
    # fallback: some older exports may use un-namespaced target
    target_link_libraries(server_storage_redis PUBLIC redis++)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++_static::redis++_static)
    target_link_libraries(server_storage_redis PUBLIC redis++_static::redis++_static)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++_static)
    target_link_libraries(server_storage_redis PUBLIC redis++_static)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
else()
    message(WARNING "redis-plus-plus not found ? Redis client stays as stub")
endif()

add_library(server_state STATIC
    src/state/instance_registry.cpp
)
target_include_directories(server_state PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_state PUBLIC server_core server_storage_redis)

add_subdirectory(plugins)
