function(configure_luajit_submodule out_target)
    if (TARGET luajit_vendor)
        set(${out_target} luajit_vendor PARENT_SCOPE)
        return()
    endif()

    set(_luajit_source_root "${LUAJIT_SUBMODULE_DIR}")
    set(_luajit_source_src "${_luajit_source_root}/src")
    set(_luajit_build_root "${CMAKE_BINARY_DIR}/third_party/luajit")
    set(_luajit_build_src "${_luajit_build_root}/src")

    if (WIN32)
        set(_luajit_output_lib "${_luajit_build_src}/lua51.lib")
        set(_luajit_build_command cmd /c msvcbuild.bat static)
        set(_luajit_primary_dep "${_luajit_source_src}/msvcbuild.bat")
    else()
        find_program(LUAJIT_MAKE_PROGRAM NAMES gmake make)
        if (NOT LUAJIT_MAKE_PROGRAM)
            message(FATAL_ERROR
                "Lua capability requires make or gmake to build upstream LuaJIT")
        endif()

        set(_luajit_output_lib "${_luajit_build_src}/libluajit.a")
        set(_luajit_build_command "${LUAJIT_MAKE_PROGRAM}" BUILDMODE=static)
        set(_luajit_primary_dep "${_luajit_source_src}/Makefile")
    endif()

    add_custom_command(
        OUTPUT "${_luajit_output_lib}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_luajit_build_root}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${_luajit_build_root}/.git"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_luajit_source_root}/.relver"
                "${_luajit_build_root}/.relver"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_luajit_source_root}/dynasm" "${_luajit_build_root}/dynasm"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_luajit_source_src}" "${_luajit_build_src}"
        COMMAND ${_luajit_build_command}
        WORKING_DIRECTORY "${_luajit_build_src}"
        DEPENDS
            "${_luajit_source_root}/.relver"
            "${_luajit_source_root}/dynasm/dynasm.lua"
            "${_luajit_source_src}/lua.h"
            "${_luajit_source_src}/luajit_rolling.h"
            "${_luajit_source_src}/ljamalg.c"
            "${_luajit_primary_dep}"
        COMMENT "Building upstream LuaJIT static library"
        VERBATIM
    )

    add_custom_target(luajit_vendor_build DEPENDS "${_luajit_output_lib}")

    file(MAKE_DIRECTORY "${_luajit_build_src}")

    add_library(luajit_vendor STATIC IMPORTED GLOBAL)
    set_target_properties(luajit_vendor PROPERTIES
        IMPORTED_LOCATION "${_luajit_output_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_luajit_build_src};${_luajit_source_src}"
    )

    if (UNIX AND NOT APPLE)
        set_property(TARGET luajit_vendor APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES "m;dl")
    endif()

    add_dependencies(luajit_vendor luajit_vendor_build)
    set(${out_target} luajit_vendor PARENT_SCOPE)
endfunction()
