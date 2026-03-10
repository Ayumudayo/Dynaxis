function(configure_sol2_submodule out_target)
    if (TARGET sol2_vendor)
        set(${out_target} sol2_vendor PARENT_SCOPE)
        return()
    endif()

    set(_sol2_include_dir "${SOL2_SUBMODULE_DIR}/include")
    if (NOT EXISTS "${_sol2_include_dir}/sol/sol.hpp")
        message(FATAL_ERROR
            "Lua capability requires sol2 headers at: ${_sol2_include_dir}. "
            "Run: git submodule update --init --recursive external/sol2")
    endif()

    add_library(sol2_vendor INTERFACE)
    target_include_directories(sol2_vendor INTERFACE
        "$<BUILD_INTERFACE:${_sol2_include_dir}>"
    )
    target_compile_definitions(sol2_vendor INTERFACE SOL_LUAJIT=1)

    set(${out_target} sol2_vendor PARENT_SCOPE)
endfunction()
