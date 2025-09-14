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
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_core wire_proto server_storage_pg server_storage_redis)
# protobuf 생성 헤더 경로 추가
target_include_directories(server_app PRIVATE ${CMAKE_BINARY_DIR}/proto)

# vcpkg DLL 자동 복사
if(WIN32)
    add_custom_command(TARGET server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotobuf$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotoc$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/abseil_dll.dll"
        $<TARGET_FILE_DIR:server_app>)
endif()

# Postgres storage adapter (skeleton)
add_library(server_storage_pg STATIC
    src/storage/postgres/connection_pool.cpp
)
target_include_directories(server_storage_pg PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_pg PUBLIC server_core)

# Try to find libpqxx (vcpkg may export either libpqxx::pqxx or unofficial::libpqxx::libpqxx)
find_package(libpqxx CONFIG QUIET)
if (libpqxx_FOUND)
    if (TARGET libpqxx::pqxx)
        target_link_libraries(server_storage_pg PUBLIC libpqxx::pqxx)
        target_compile_definitions(server_storage_pg PUBLIC HAVE_LIBPQXX)
    endif()
else()
    find_package(unofficial-libpqxx CONFIG QUIET)
    if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
        target_link_libraries(server_storage_pg PUBLIC unofficial::libpqxx::libpqxx)
        target_compile_definitions(server_storage_pg PUBLIC HAVE_LIBPQXX)
    else()
        message(WARNING "libpqxx not found — PG adapter will stay as stub (no real DB ops)")
    endif()
endif()

# Redis client/pool (skeleton)
add_library(server_storage_redis STATIC
    src/storage/redis/client.cpp
)
target_include_directories(server_storage_redis PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_redis PUBLIC server_core)

# Try to find redis-plus-plus; targets may be redis++ or redis++_static
find_package(redis++ CONFIG QUIET)
if (TARGET redis++)
    target_link_libraries(server_storage_redis PUBLIC redis++)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++_static)
    target_link_libraries(server_storage_redis PUBLIC redis++_static)
    target_compile_definitions(server_storage_redis PUBLIC HAVE_REDIS_PLUS_PLUS)
else()
    message(WARNING "redis-plus-plus not found — Redis client stays as stub")
endif()
