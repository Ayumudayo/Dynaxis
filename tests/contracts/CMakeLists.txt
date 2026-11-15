if (NOT BUILD_CONTRACT_TESTS)
    return()
endif()
add_executable(core_public_api_headers_compile ../core/public_api_headers_compile.cpp)
target_link_libraries(core_public_api_headers_compile PRIVATE server_core)
add_test(NAME CorePublicApiHeadersCompile COMMAND core_public_api_headers_compile)
add_executable(server_chat_service_headers_compile server_chat_service_headers_compile.cpp)
target_include_directories(server_chat_service_headers_compile PRIVATE ${CMAKE_SOURCE_DIR}/server/include)
target_link_libraries(server_chat_service_headers_compile PRIVATE server_core)
add_test(NAME ServerChatServiceHeadersCompile COMMAND server_chat_service_headers_compile)
if (Python3_Interpreter_FOUND)
    add_test(NAME DoxygenCoverage COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_doxygen_coverage.py)
    add_test(NAME OpcodeContract COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_opcode_docs.py --check)
    add_test(NAME CoreApiBoundary COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py --check-boundary)
endif()

add_executable(core_public_api_smoke ../core/public_api_smoke.cpp)
target_link_libraries(core_public_api_smoke PRIVATE server_core)
add_test(NAME CorePublicApiSmoke COMMAND core_public_api_smoke)
    add_test(NAME CoreInstalledPackageConsumer
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/installed_consumer"
            "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/full-consumer/build"
            "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/full-consumer/prefix"
            "-DSERVER_CORE_PACKAGE_DIR=${CMAKE_BINARY_DIR}/contract/full-consumer/prefix/${CMAKE_INSTALL_LIBDIR}/cmake/server_core"
            "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCONFIG=$<CONFIG>"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_package_consumer.cmake")
    set_tests_properties(CoreInstalledPackageConsumer PROPERTIES LABELS "core-runtime;installed-consumer" TIMEOUT 120)

if(TARGET server_storage_pg_factory AND TARGET infra_redis_factory)
add_test(
  NAME FactoryPgInstalledPackageConsumer
  COMMAND "${CMAKE_COMMAND}"
          "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/factory_pg_consumer"
          "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/factory-pg-consumer/build"
          "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/factory-pg-consumer/prefix"
          "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
          "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          "-DCONFIG=$<CONFIG>"
          "-DGENERATOR=${CMAKE_GENERATOR}"
          "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
          "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
          -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_factory_package_consumer.cmake"
)
set_tests_properties(FactoryPgInstalledPackageConsumer PROPERTIES LABELS "contract;package;installed-consumer")

add_test(
  NAME FactoryRedisInstalledPackageConsumer
  COMMAND "${CMAKE_COMMAND}"
          "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/factory_redis_consumer"
          "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/factory-redis-consumer/build"
          "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/factory-redis-consumer/prefix"
          "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
          "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          "-DCONFIG=$<CONFIG>"
          "-DGENERATOR=${CMAKE_GENERATOR}"
          "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
          "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
          -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_factory_package_consumer.cmake"
)
set_tests_properties(FactoryRedisInstalledPackageConsumer PROPERTIES LABELS "contract;package;installed-consumer")

endif()
