add_executable(server_app
    src/main.cpp
    src/app/bootstrap.cpp
    src/app/router.cpp
    src/chat/chat_service_core.cpp
    src/chat/handlers_login.cpp
    src/chat/session_events.cpp
    src/chat/handlers_join.cpp
    src/chat/handlers_leave.cpp
    src/chat/handlers_chat.cpp
    src/chat/handlers_ping.cpp
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_core wire_proto server_storage_pg server_storage_redis server_state)
# protobuf 생성 헤더 경로 추가
target_include_directories(server_app PRIVATE ${CMAKE_BINARY_DIR}/proto)

# vcpkg DLL 자동 복사(Windows)
if(WIN32)
    add_custom_command(TARGET server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotobuf$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotoc$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/abseil_dll.dll"
        $<TARGET_FILE_DIR:server_app>)
endif()

# Postgres storage adapter
add_library(server_storage_pg STATIC
    src/storage/postgres/connection_pool.cpp
)
target_include_directories(server_storage_pg PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_pg PUBLIC server_core)

# libpqxx 필수: vcpkg가 libpqxx::pqxx 또는 unofficial::libpqxx::libpqxx를 제공
find_package(libpqxx CONFIG QUIET)
if (libpqxx_FOUND AND TARGET libpqxx::pqxx)
    target_link_libraries(server_storage_pg PUBLIC libpqxx::pqxx)
else()
    find_package(unofficial-libpqxx CONFIG QUIET)
    if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
        target_link_libraries(server_storage_pg PUBLIC unofficial::libpqxx::libpqxx)
    else()
        message(FATAL_ERROR "libpqxx not found — PostgreSQL storage requires libpqxx")
    endif()
endif()

# Redis client/pool (skeleton)
add_library(server_storage_redis STATIC
    src/storage/redis/client.cpp
)
target_include_directories(server_storage_redis PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_redis PUBLIC server_core)

# Try to find redis-plus-plus; vcpkg exports namespaced targets
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
