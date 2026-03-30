if (NOT BUILD_CONTRACT_TESTS)
    return()
endif()

if (NOT CMAKE_INSTALL_LIBDIR)
    set(CMAKE_INSTALL_LIBDIR "lib")
endif()

add_executable(core_public_api_smoke
    ../core/public_api_smoke.cpp
)
target_include_directories(core_public_api_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(core_public_api_smoke PRIVATE
    server_core
)
add_test(NAME CorePublicApiSmoke COMMAND core_public_api_smoke)
set_tests_properties(CorePublicApiSmoke PROPERTIES LABELS "contract;public-api;smoke")

add_executable(core_public_api_headers_compile
    ../core/public_api_headers_compile.cpp
)
target_include_directories(core_public_api_headers_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(core_public_api_headers_compile PRIVATE
    server_core
)
add_test(NAME CorePublicApiHeadersCompile COMMAND core_public_api_headers_compile)
set_tests_properties(CorePublicApiHeadersCompile PROPERTIES LABELS "contract;public-api;compile")

if (TARGET server_core)
    add_executable(server_chat_service_headers_compile
        server_chat_service_headers_compile.cpp
    )
    target_include_directories(server_chat_service_headers_compile PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/server/include
    )
    target_link_libraries(server_chat_service_headers_compile PRIVATE
        server_core
    )
    add_test(NAME ServerChatServiceHeadersCompile COMMAND server_chat_service_headers_compile)
    set_tests_properties(ServerChatServiceHeadersCompile PROPERTIES LABELS "contract;public-api;compile")
endif()

add_executable(core_public_api_stable_header_scenarios
    ../core/public_api_stable_header_scenarios.cpp
)
target_include_directories(core_public_api_stable_header_scenarios PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(core_public_api_stable_header_scenarios PRIVATE
    server_core
)
add_test(NAME CorePublicApiStableHeaderScenarios COMMAND core_public_api_stable_header_scenarios)
set_tests_properties(CorePublicApiStableHeaderScenarios PROPERTIES LABELS "contract;public-api;smoke")

add_executable(core_public_api_realtime_capability_smoke
    ../core/public_api_realtime_capability_smoke.cpp
)
target_include_directories(core_public_api_realtime_capability_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(core_public_api_realtime_capability_smoke PRIVATE
    server_core
)
add_test(NAME CorePublicApiRealtimeCapabilitySmoke COMMAND core_public_api_realtime_capability_smoke)
set_tests_properties(CorePublicApiRealtimeCapabilitySmoke PROPERTIES LABELS "contract;public-api;realtime")

add_executable(core_public_api_worlds_aws_smoke
    ../core/public_api_worlds_aws_smoke.cpp
)
target_include_directories(core_public_api_worlds_aws_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(core_public_api_worlds_aws_smoke PRIVATE
    server_core
)
add_test(NAME CorePublicApiWorldsAwsSmoke COMMAND core_public_api_worlds_aws_smoke)
set_tests_properties(CorePublicApiWorldsAwsSmoke PROPERTIES LABELS "contract;public-api;worlds;provider")

if (TARGET test_plugin_runtime_v1 AND TARGET test_plugin_runtime_v2)
    add_executable(core_public_api_extensibility_smoke
        ../core/public_api_extensibility_smoke.cpp
    )
    target_include_directories(core_public_api_extensibility_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    target_compile_definitions(core_public_api_extensibility_smoke PRIVATE
        TEST_PLUGIN_RUNTIME_V1_FILE="$<TARGET_FILE_NAME:test_plugin_runtime_v1>"
        TEST_PLUGIN_RUNTIME_V2_FILE="$<TARGET_FILE_NAME:test_plugin_runtime_v2>"
    )
    target_link_libraries(core_public_api_extensibility_smoke PRIVATE
        server_core
        ${CMAKE_DL_LIBS}
    )
    add_dependencies(core_public_api_extensibility_smoke
        test_plugin_runtime_v1
        test_plugin_runtime_v2
    )
    add_test(
      NAME CorePublicApiExtensibilitySmoke
      COMMAND "${CMAKE_COMMAND}"
              -E chdir "$<TARGET_FILE_DIR:test_plugin_runtime_v1>"
              "$<TARGET_FILE:core_public_api_extensibility_smoke>"
    )
    set_tests_properties(CorePublicApiExtensibilitySmoke PROPERTIES LABELS "contract;public-api;extensibility")
endif()

add_test(
  NAME CoreInstalledPackageConsumer
  COMMAND "${CMAKE_COMMAND}"
          "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
          "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/installed_consumer"
          "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/installed-consumer/build"
          "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/installed-consumer/prefix"
          "-DSERVER_CORE_PACKAGE_DIR=${CMAKE_BINARY_DIR}/contract/installed-consumer/prefix/${CMAKE_INSTALL_LIBDIR}/cmake/server_core"
          "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
          "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          "-DCONFIG=$<CONFIG>"
          "-DGENERATOR=${CMAKE_GENERATOR}"
          "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
          "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
          -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_package_consumer.cmake"
)
set_tests_properties(CoreInstalledPackageConsumer PROPERTIES LABELS "contract;public-api;installed-consumer")

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

if (Python3_Interpreter_FOUND)
  add_test(
    NAME CoreApiBoundaryFixtures
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-boundary-fixtures
  )
  set_tests_properties(CoreApiBoundaryFixtures PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME DoxygenCoverageToolTests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_check_doxygen_coverage.py"
  )
  set_tests_properties(DoxygenCoverageToolTests PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME WindowsValidationLayoutTests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_windows_validation_layout.py"
  )
  set_tests_properties(WindowsValidationLayoutTests PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME PackageConsumerContractTests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_package_consumer_contracts.py"
  )
  set_tests_properties(PackageConsumerContractTests PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME GatewayPublicHeaderContracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_gateway_public_header_contracts.py"
  )
  set_tests_properties(GatewayPublicHeaderContracts PROPERTIES LABELS "contract;public-api;gateway")
  add_test(
    NAME WindowsReleaseTreeReady
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_windows_release_tree_ready.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
  )
  set_tests_properties(WindowsReleaseTreeReady PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME DeployDockerBuildPlanTests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_deploy_docker_build_plan.py"
  )
  set_tests_properties(DeployDockerBuildPlanTests PROPERTIES LABELS "contract;docs-policy")
  add_test(
    NAME CoreApiStableGovernanceFixtures
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-stable-governance-fixtures
  )
  set_tests_properties(CoreApiStableGovernanceFixtures PROPERTIES LABELS "contract;docs-policy")
else()
  message(WARNING "Python3 not found: CoreApiBoundaryFixtures test disabled")
endif()
