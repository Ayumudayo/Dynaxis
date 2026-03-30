if (BUILD_WRITE_BEHIND_TOOLS)
  add_library(wb_common_redis_factory STATIC wb_common/redis_client_factory.cpp)
  target_link_libraries(wb_common_redis_factory PUBLIC server_core)
  target_link_libraries(wb_common_redis_factory PRIVATE infra_redis_factory)

  add_executable(wb_worker wb_worker/main.cpp)
  add_executable(wb_dlq_replayer wb_dlq_replayer/main.cpp)
  add_executable(wb_emit wb_emit/main.cpp)
  add_executable(wb_check wb_check/main.cpp)

  target_link_libraries(wb_worker PRIVATE server_core)
  target_link_libraries(wb_dlq_replayer PRIVATE server_core)
  target_link_libraries(wb_emit PRIVATE server_core)
  target_link_libraries(wb_check PRIVATE server_core)

  target_link_libraries(wb_worker PRIVATE wb_common_redis_factory)
  target_link_libraries(wb_dlq_replayer PRIVATE wb_common_redis_factory)
  target_link_libraries(wb_emit PRIVATE wb_common_redis_factory)

  if (TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(wb_worker PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(wb_dlq_replayer PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(wb_check PRIVATE nlohmann_json::nlohmann_json)
  else()
    message(WARNING "nlohmann_json target not found ? wb_worker/wb_dlq_replayer/wb_check may fail to compile")
  endif()

  set(_PQXX_TARGET "")
  find_package(libpqxx CONFIG QUIET)
  if (libpqxx_FOUND AND TARGET libpqxx::pqxx)
    set(_PQXX_TARGET libpqxx::pqxx)
  else()
    pkg_check_modules(libpqxx QUIET IMPORTED_TARGET libpqxx)
    if (libpqxx_FOUND)
      set(_PQXX_TARGET PkgConfig::libpqxx)
    else()
      find_package(unofficial-libpqxx CONFIG QUIET)
      if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
        set(_PQXX_TARGET unofficial::libpqxx::libpqxx)
      endif()
    endif()
  endif()

  if (_PQXX_TARGET)
    target_link_libraries(wb_worker PRIVATE ${_PQXX_TARGET})
    target_link_libraries(wb_dlq_replayer PRIVATE ${_PQXX_TARGET})
    target_link_libraries(wb_check PRIVATE ${_PQXX_TARGET})
  else()
    message(WARNING "libpqxx not found ? wb_worker/wb_dlq_replayer/wb_check will fail to link")
  endif()
endif()

if (BUILD_LOADGEN_TOOLS)
  if (NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "nlohmann_json target not found ? stack_loadgen requires nlohmann_json")
  endif()

  add_executable(stack_loadgen
    loadgen/main.cpp
    loadgen/report_writer.cpp
    loadgen/scenario_loader.cpp
    loadgen/scenario_runner.cpp
    loadgen/session_client.cpp
  )
  target_include_directories(stack_loadgen PRIVATE
    ${CMAKE_BINARY_DIR}/proto
  )
  target_link_libraries(stack_loadgen PRIVATE
    server_core
    wire_proto
    nlohmann_json::nlohmann_json
  )

  add_custom_target(tcp_loadgen DEPENDS stack_loadgen)

  if (WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET stack_loadgen POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              -Druntime_dlls=$<TARGET_RUNTIME_DLLS:stack_loadgen>
              -Dtarget_dir=$<TARGET_FILE_DIR:stack_loadgen>
              -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls_if_any.cmake
      COMMAND_EXPAND_LISTS)
  endif()
endif()

if (BUILD_ADMIN_APP)
  add_library(admin_app_backends STATIC admin_app/redis_client_factory.cpp)
  target_link_libraries(admin_app_backends PUBLIC server_core)
  target_link_libraries(admin_app_backends PRIVATE infra_redis_factory)

  add_executable(admin_app admin_app/main.cpp)
  target_link_libraries(admin_app PRIVATE server_core)
  target_include_directories(admin_app PRIVATE ${CMAKE_SOURCE_DIR}/server/include)
  if (TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(admin_app PRIVATE nlohmann_json::nlohmann_json)
  else()
    message(WARNING "nlohmann_json target not found ? admin_app may fail to compile")
  endif()
  target_link_libraries(admin_app PRIVATE admin_app_backends)

  add_custom_command(TARGET admin_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/admin_app/admin_ui.html"
            "$<TARGET_FILE_DIR:admin_app>/admin_ui.html")

  if (WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET admin_app POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              -Druntime_dlls=$<TARGET_RUNTIME_DLLS:admin_app>
              -Dtarget_dir=$<TARGET_FILE_DIR:admin_app>
              -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls_if_any.cmake
      COMMAND_EXPAND_LISTS)
  endif()
endif()

if (BUILD_MIGRATIONS_RUNNER)
  add_executable(migrations_runner migrations/runner.cpp)
  target_include_directories(migrations_runner PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

  find_package(libpqxx CONFIG QUIET)
  if (libpqxx_FOUND AND TARGET libpqxx::pqxx)
    target_link_libraries(migrations_runner PRIVATE libpqxx::pqxx)
  else()
    pkg_check_modules(libpqxx QUIET IMPORTED_TARGET libpqxx)
    if (libpqxx_FOUND)
      target_link_libraries(migrations_runner PRIVATE PkgConfig::libpqxx)
    else()
      find_package(unofficial-libpqxx CONFIG QUIET)
      if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
        target_link_libraries(migrations_runner PRIVATE unofficial::libpqxx::libpqxx)
      else()
        message(WARNING "libpqxx not found — migrations_runner will fail to link")
      endif()
    endif()
  endif()
endif()
