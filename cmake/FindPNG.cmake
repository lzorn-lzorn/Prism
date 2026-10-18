# FindPNG.cmake
#
# Resolves `find_package(PNG)` to the libpng compiled from the sources in
# extern/libpng instead of a system library. cmake/Dependencies.cmake puts this
# directory on CMAKE_MODULE_PATH, so this module is always preferred over
# CMake's built-in FindPNG and no system libpng is ever searched.
#
# Consumers: FreeType, for PNG compressed color bitmap glyphs (pngshim.c).

if(NOT TARGET PNG::PNG)
    if(TARGET png_static)
        add_library(PNG::PNG ALIAS png_static)
    else()
        message(FATAL_ERROR
            "find_package(PNG) needs the libpng built from extern/libpng. "
            "Add that subdirectory before its consumers in cmake/Dependencies.cmake.")
    endif()
endif()

set(PNG_LIBRARIES PNG::PNG)
set(PNG_LIBRARY PNG::PNG)
set(PNG_PNG_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../extern/libpng")
get_target_property(PNG_INCLUDE_DIRS PNG::PNG INTERFACE_INCLUDE_DIRECTORIES)
if(PNG_INCLUDE_DIRS MATCHES "-NOTFOUND$")
    set(PNG_INCLUDE_DIRS)
endif()

# Report the version of the vendored sources rather than the libpng CMake target.
# The vendored header may carry a suffix ("1.6.38.git"), so keep only the leading
# numeric part.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../extern/libpng/png.h" EditorPngVersionLine
    REGEX "^#define[ \t]+PNG_LIBPNG_VER_STRING[ \t]+\"[0-9]")
if(EditorPngVersionLine)
    string(REGEX REPLACE "^.*\"([0-9]+(\\.[0-9]+)*).*$" "\\1" PNG_VERSION_STRING "${EditorPngVersionLine}")
    set(PNG_VERSION "${PNG_VERSION_STRING}")
endif()
unset(EditorPngVersionLine)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PNG
    REQUIRED_VARS PNG_LIBRARIES
    VERSION_VAR PNG_VERSION_STRING)
