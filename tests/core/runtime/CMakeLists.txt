if (NOT CMAKE_INSTALL_LIBDIR)
    set(CMAKE_INSTALL_LIBDIR "lib")
endif()

# 최소 런타임 검증은 GTest와 MSA 앱/생성 프로토콜을 요구하지 않는다.
add_executable(core_runtime_smoke runtime_smoke.cpp)
target_link_libraries(core_runtime_smoke PRIVATE server_core)
add_test(NAME CoreRuntimeSmoke COMMAND core_runtime_smoke)
set_tests_properties(CoreRuntimeSmoke PROPERTIES LABELS "core-runtime;contract;smoke" TIMEOUT 10)

if (NOT BUILD_CORE_EXTENSIONS AND BUILD_CONTRACT_TESTS)
    add_test(NAME CoreRuntimeInstalledConsumer
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/runtime_consumer"
            "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/runtime-consumer/build"
            "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/runtime-consumer/prefix"
            "-DSERVER_CORE_PACKAGE_DIR=${CMAKE_BINARY_DIR}/contract/runtime-consumer/prefix/${CMAKE_INSTALL_LIBDIR}/cmake/server_core"
            "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCONFIG=$<CONFIG>"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_package_consumer.cmake")
    set_tests_properties(CoreRuntimeInstalledConsumer PROPERTIES LABELS "core-runtime;installed-consumer" TIMEOUT 120)
endif()
