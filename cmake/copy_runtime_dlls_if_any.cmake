if(NOT DEFINED target_dir OR target_dir STREQUAL "")
    message(FATAL_ERROR "target_dir is required")
endif()

if(NOT DEFINED runtime_dlls OR runtime_dlls STREQUAL "")
    return()
endif()

foreach(runtime_dll IN LISTS runtime_dlls)
    if(NOT runtime_dll STREQUAL "" AND EXISTS "${runtime_dll}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_dll}" "${target_dir}"
            RESULT_VARIABLE copy_result
        )
        if(NOT copy_result EQUAL 0)
            message(FATAL_ERROR "Failed to copy runtime DLL: ${runtime_dll}")
        endif()
    endif()
endforeach()
