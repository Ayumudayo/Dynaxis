cmake_minimum_required(VERSION 3.20)

foreach(required_var BUILD_DIR CONSUMER_SOURCE_DIR CONSUMER_BUILD_DIR INSTALL_PREFIX CONFIG GENERATOR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_installed_factory_package_consumer.cmake missing required variable: ${required_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${CONSUMER_BUILD_DIR}")
file(MAKE_DIRECTORY "${CONSUMER_BUILD_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}" --config "${CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "factory package install failed\n${install_stdout}\n${install_stderr}")
endif()

set(configure_cmd
    "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${CONSUMER_BUILD_DIR}"
    -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}"
)

if(DEFINED TOOLCHAIN_FILE AND NOT "${TOOLCHAIN_FILE}" STREQUAL "")
    list(APPEND configure_cmd "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
if(DEFINED MAKE_PROGRAM AND NOT "${MAKE_PROGRAM}" STREQUAL "")
    list(APPEND configure_cmd "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(DEFINED GENERATOR_PLATFORM AND NOT "${GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_cmd -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT "${GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_cmd -T "${GENERATOR_TOOLSET}")
endif()

execute_process(
    COMMAND ${configure_cmd}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "factory package consumer configure failed\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}" --config "${CONFIG}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "factory package consumer build failed\n${build_stdout}\n${build_stderr}")
endif()
