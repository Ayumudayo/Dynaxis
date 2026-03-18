cmake_minimum_required(VERSION 3.20)

foreach(required_var BUILD_DIR CONSUMER_SOURCE_DIR CONSUMER_BUILD_DIR INSTALL_PREFIX SERVER_CORE_PACKAGE_DIR CONFIG GENERATOR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_installed_package_consumer.cmake missing required variable: ${required_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${CONSUMER_BUILD_DIR}")
file(REMOVE_RECURSE "${INSTALL_PREFIX}")
file(MAKE_DIRECTORY "${CONSUMER_BUILD_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}" --config "${CONFIG}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "server_core install failed\n${install_stdout}\n${install_stderr}")
endif()

set(configure_cmd
    "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${CONSUMER_BUILD_DIR}"
    -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
    "-Dserver_core_DIR=${SERVER_CORE_PACKAGE_DIR}"
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
    message(FATAL_ERROR "installed consumer configure failed\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}" --config "${CONFIG}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "installed consumer build failed\n${build_stdout}\n${build_stderr}")
endif()

function(run_consumer_executable target_name)
    set(candidates
        "${CONSUMER_BUILD_DIR}/${target_name}"
        "${CONSUMER_BUILD_DIR}/${CONFIG}/${target_name}"
        "${CONSUMER_BUILD_DIR}/${target_name}.exe"
        "${CONSUMER_BUILD_DIR}/${CONFIG}/${target_name}.exe"
    )

    set(target_path "")
    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}")
            set(target_path "${candidate}")
            break()
        endif()
    endforeach()

    if("${target_path}" STREQUAL "")
        message(FATAL_ERROR "installed consumer executable not found for ${target_name}")
    endif()

    execute_process(
        COMMAND "${target_path}"
        WORKING_DIRECTORY "${CONSUMER_BUILD_DIR}"
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_stdout
        ERROR_VARIABLE run_stderr
    )
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "installed consumer run failed for ${target_name}\n${run_stdout}\n${run_stderr}")
    endif()
endfunction()

run_consumer_executable("server_core_installed_consumer")
run_consumer_executable("server_core_extensibility_consumer")
