# FindZLIB.cmake
#
# Resolves `find_package(ZLIB)` to the zlib compiled from the sources in
# extern/zlib instead of a system library. cmake/Dependencies.cmake puts this
# directory on CMAKE_MODULE_PATH, so this module is always preferred over
# CMake's built-in FindZLIB and no system zlib is ever searched.
#
# Consumers: the vendored libpng, FreeType (system zlib for its gzip tables)
# and, when enabled, HarfBuzz's vector backend.

if(NOT TARGET ZLIB::ZLIB)
    if(TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    else()
        message(FATAL_ERROR
            "find_package(ZLIB) needs the zlib built from extern/zlib. "
            "Add that subdirectory before its consumers in cmake/Dependencies.cmake.")
    endif()
endif()

set(ZLIB_LIBRARIES ZLIB::ZLIB)
set(ZLIB_LIBRARY ZLIB::ZLIB)
set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../extern/zlib")
get_target_property(EditorZlibBinaryDir zlibstatic BINARY_DIR)
set(ZLIB_INCLUDE_DIRS "${EditorZlibBinaryDir}" "${ZLIB_INCLUDE_DIR}")

# Report the version of the vendored sources rather than the zlib CMake target.
# The vendored header may carry a suffix ("1.3.2.1-motley"), so keep only the
# leading numeric part.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../extern/zlib/zlib.h" EditorZlibVersionLine
    REGEX "^#define[ \t]+ZLIB_VERSION[ \t]+\"[0-9]")
if(EditorZlibVersionLine)
    string(REGEX REPLACE "^.*\"([0-9]+(\\.[0-9]+)*).*$" "\\1" ZLIB_VERSION_STRING "${EditorZlibVersionLine}")
    set(ZLIB_VERSION "${ZLIB_VERSION_STRING}")
endif()
unset(EditorZlibVersionLine)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZLIB
    REQUIRED_VARS ZLIB_LIBRARIES
    VERSION_VAR ZLIB_VERSION_STRING)
