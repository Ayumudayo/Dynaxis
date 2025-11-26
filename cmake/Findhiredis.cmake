find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_HIREDIS QUIET hiredis)

find_path(HIREDIS_INCLUDE_DIR
    NAMES hiredis/hiredis.h
    PATHS ${PC_HIREDIS_INCLUDE_DIRS}
)

find_library(HIREDIS_LIBRARY
    NAMES hiredis
    PATHS ${PC_HIREDIS_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(hiredis
    DEFAULT_MSG
    HIREDIS_LIBRARY HIREDIS_INCLUDE_DIR
)

if(hiredis_FOUND)
    set(HIREDIS_LIBRARIES ${HIREDIS_LIBRARY})
    set(HIREDIS_INCLUDE_DIRS ${HIREDIS_INCLUDE_DIR})
    if(NOT TARGET hiredis::hiredis)
        add_library(hiredis::hiredis UNKNOWN IMPORTED)
        set_target_properties(hiredis::hiredis PROPERTIES
            IMPORTED_LOCATION "${HIREDIS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}"
        )
    endif()
endif()
