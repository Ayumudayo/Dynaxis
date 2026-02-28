cmake_minimum_required(VERSION 3.20)
project(ServerProject LANGUAGES CXX)

option(SERVER_ENABLE_TLS "Enable TLS(Asio SSL)" OFF)
option(SERVER_ENABLE_PROFILING "Enable profiling markers" OFF)
option(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM "Use header-only boost::system (no lib link)" OFF)
option(KNIGHTS_ENABLE_SANITIZERS "Enable sanitizers (ASan+UBSan)" OFF)
option(KNIGHTS_ENABLE_IPO "Enable interprocedural optimization (IPO/LTO)" OFF)
option(KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS "Enable gateway UDP ingress feature path" ON)
option(KNIGHTS_ENABLE_CORE_RUDP "Enable core RUDP engine path" OFF)
option(BUILD_SERVER_STACK "Build server libraries and server_app" ON)
option(BUILD_GATEWAY_APP "Build gateway_app" ON)


option(BUILD_SERVER_TESTS "Build server tests" ON)
option(BUILD_GTEST_TESTS "Build GTest-based unit tests" ON)
option(BUILD_CONTRACT_TESTS "Build non-GTest contract/governance checks" ON)
option(BUILD_WRITE_BEHIND_TOOLS "Build write-behind helper tools" ON)
option(BUILD_ADMIN_APP "Build admin management app" ON)
option(BUILD_MIGRATIONS_RUNNER "Build migrations runner tool" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")



if (MSVC)
    add_compile_options(/utf-8 /MP)
endif()

if (KNIGHTS_ENABLE_SANITIZERS)
    message(STATUS "Sanitizers enabled (ASan+UBSan where supported)")

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    elseif (MSVC)
        # NOTE: MSVC ASan support depends on toolchain/runtime; keep opt-in.
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
    else()
        message(WARNING "KNIGHTS_ENABLE_SANITIZERS=ON but unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if (KNIGHTS_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT KNIGHTS_IPO_SUPPORTED OUTPUT KNIGHTS_IPO_OUTPUT)
    if (KNIGHTS_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "IPO/LTO enabled (KNIGHTS_ENABLE_IPO=ON)")
    else()
        message(WARNING "KNIGHTS_ENABLE_IPO=ON but IPO is not supported: ${KNIGHTS_IPO_OUTPUT}")
    endif()
endif()

if (NOT BUILD_SERVER_STACK)

    if (BUILD_WRITE_BEHIND_TOOLS)
        message(FATAL_ERROR "write-behind tools require BUILD_SERVER_STACK=ON")
    endif()
endif()


# Windows에서 Boost.Asio가 적절한 WinSock API를 사용하도록 타깃에 매크로 부여 예정

set(Boost_USE_STATIC_LIBS OFF CACHE BOOL "link with boost static libs")
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    find_package(Boost REQUIRED)
else()
    find_package(Boost QUIET COMPONENTS system)
    if (NOT Boost_FOUND)
        message(WARNING "Boost::system을 찾지 못했습니다. header-only 모드로 폴백합니다. (BOOST_ERROR_CODE_HEADER_ONLY)")
        set(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM ON CACHE BOOL "Use header-only boost::system" FORCE)
        find_package(Boost REQUIRED)
    endif()
endif()

message(STATUS "Boost header-only-system: ${SERVER_USE_HEADER_ONLY_BOOST_SYSTEM}")
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libs: ${Boost_LIBRARIES}")
message(STATUS "Boost lib dirs: ${Boost_LIBRARY_DIRS}")

# Python3가 있으면 opcode 헤더를 빌드 시 자동 생성
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    set(OPCODES_JSON ${CMAKE_SOURCE_DIR}/core/protocol/system_opcodes.json)
    set(GENERATED_HEADER ${CMAKE_SOURCE_DIR}/core/include/server/core/protocol/system_opcodes.hpp)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py ${OPCODES_JSON} ${GENERATED_HEADER}
        DEPENDS ${OPCODES_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py
        COMMENT "Generating protocol system opcodes header"
        VERBATIM)
    add_custom_target(generate_opcodes DEPENDS ${GENERATED_HEADER})
    # wire codec header generation
    set(WIRE_MAP_JSON ${CMAKE_SOURCE_DIR}/protocol/wire_map.json)
    set(GENERATED_WIRE_CODEC ${CMAKE_SOURCE_DIR}/core/include/server/wire/codec.hpp)
    add_custom_command(
        OUTPUT ${GENERATED_WIRE_CODEC}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py ${WIRE_MAP_JSON} ${GENERATED_WIRE_CODEC}
        DEPENDS ${WIRE_MAP_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py ${CMAKE_SOURCE_DIR}/proto/wire.proto
        COMMENT "Generating wire codec header"
        VERBATIM)
    add_custom_target(generate_wire_codec DEPENDS ${GENERATED_WIRE_CODEC})
else()
    message(WARNING "Python3 not found: opcode 자동 생성 비활성화. 기존 헤더를 사용합니다.")
endif()

add_subdirectory(core)
add_subdirectory(proto)
if (BUILD_SERVER_STACK)
  add_subdirectory(server)
endif()
if (BUILD_GATEWAY_APP)
  add_subdirectory(gateway)
endif()


if (BUILD_SERVER_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if (BUILD_WRITE_BEHIND_TOOLS)
  add_executable(wb_worker tools/wb_worker/main.cpp)
  add_executable(wb_dlq_replayer tools/wb_dlq_replayer/main.cpp)
  add_executable(wb_emit tools/wb_emit/main.cpp)
  add_executable(wb_check tools/wb_check/main.cpp)

  target_link_libraries(wb_worker PRIVATE server_core)
  target_link_libraries(wb_dlq_replayer PRIVATE server_core)
  target_link_libraries(wb_emit PRIVATE server_core)
  target_link_libraries(wb_check PRIVATE server_core)

  if (TARGET server_storage_pg)
    target_link_libraries(wb_worker PRIVATE server_storage_pg)
    target_link_libraries(wb_check PRIVATE server_storage_pg)
  endif()
  if (TARGET server_storage_redis)
    target_link_libraries(wb_worker PRIVATE server_storage_redis)
    target_link_libraries(wb_dlq_replayer PRIVATE server_storage_redis)
    target_link_libraries(wb_emit PRIVATE server_storage_redis)
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

if (BUILD_ADMIN_APP)
  add_executable(admin_app tools/admin_app/main.cpp)
  target_link_libraries(admin_app PRIVATE server_core)
  if (TARGET server_storage_redis)
    target_link_libraries(admin_app PRIVATE server_storage_redis)
  endif()
  if (TARGET server_state)
    target_link_libraries(admin_app PRIVATE server_state)
  endif()

  add_custom_command(TARGET admin_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/admin_app/admin_ui.html"
            "$<TARGET_FILE_DIR:admin_app>/admin_ui.html")

  if (WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    # Ensure local execution works without manually extending PATH.
    add_custom_command(TARGET admin_app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:admin_app>
              $<TARGET_FILE_DIR:admin_app>
      COMMAND_EXPAND_LISTS)

    # Copy release bin first (contains hiredis.dll required by some debug import chains).
    add_custom_command(TARGET admin_app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin"
              "$<TARGET_FILE_DIR:admin_app>")

    # Some transitive runtime DLLs (e.g. OpenSSL legacy/zlib chain) are not
    # always surfaced via imported target metadata; mirror vcpkg bin for safety.
    add_custom_command(TARGET admin_app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin"
              "$<TARGET_FILE_DIR:admin_app>")
  endif()
endif()

if (BUILD_MIGRATIONS_RUNNER)
  # migrations runner (requires libpqxx)
  add_executable(migrations_runner tools/migrations/runner.cpp)
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

if (WIN32)
    add_subdirectory(client_gui)
endif()
