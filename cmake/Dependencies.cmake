include_guard(GLOBAL)
include(FetchContent)

# The vendored find modules beside this file map find_package(PNG) and
# find_package(ZLIB) onto the sources in extern, never onto system packages.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

# CLI11: 命令行解析(原 Editor CommandLineParser 依赖), 已 vendor 到 extern/cli11.
function(seed_add_cli11)
    if(NOT TARGET CLI11::CLI11)
        add_subdirectory("${PROJECT_SOURCE_DIR}/extern/cli11"
            "${PROJECT_BINARY_DIR}/extern/cli11" EXCLUDE_FROM_ALL)
    endif()
endfunction()

function(seed_add_source_dependencies)
    foreach(Dependency IN ITEMS zlib libpng freetype harfbuzz earcut)
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/extern/${Dependency}/CMakeLists.txt")
            message(FATAL_ERROR
                "Missing extern/${Dependency} source. Run git submodule update --init --recursive.")
        endif()
    endforeach()

    # Scoped to this function, not a forced global BUILD_SHARED_LIBS cache value.
    # SDL keeps its separately configured SDL_SHARED/SDL_STATIC choices.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_FRAMEWORK OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Sources under extern/ are vendored dependencies, not installable products.
    # Dropping their install rules also keeps their export sets from rejecting
    # the vendored-target links below (for example libpng linking zlibstatic).
    # The variable stays inside this function, so the editor targets keep the
    # ability to add install rules later.
    set(CMAKE_SKIP_INSTALL_RULES TRUE)

    # zlib is built from extern/zlib and is shared by libpng and FreeType below.
    # Only the static variant is kept, so no system zlib is searched and no extra
    # runtime DLL appears next to the executables.
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Build only the static zlib" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "Build the static zlib" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Disable zlib example programs" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "Skip zlib install rules" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/zlib" "${PROJECT_BINARY_DIR}/extern/zlib" EXCLUDE_FROM_ALL)

    # libpng is built from extern/libpng and consumes that zlib through
    # find_package(ZLIB), which resolves to cmake/FindZLIB.cmake.
    set(PNG_SHARED OFF CACHE BOOL "Build only the static libpng" FORCE)
    set(PNG_STATIC ON CACHE BOOL "Build the static libpng" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "Disable libpng tests" FORCE)
    unset(PNG_EXECUTABLES CACHE)
    set(PNG_TOOLS OFF CACHE BOOL "Disable libpng tools" FORCE)
    unset(PNG_ARM_NEON CACHE)
    file(SHA256 "${PROJECT_SOURCE_DIR}/extern/zlib/zlib.h" EditorZlibHeaderHash)
    file(SHA256 "${PROJECT_SOURCE_DIR}/extern/libpng/png.h" EditorPngHeaderHash)
    file(SHA256 "${PROJECT_SOURCE_DIR}/cmake/FindZLIB.cmake" EditorZlibFinderHash)
    string(SHA256 EditorPngConfiguration "${EditorZlibHeaderHash};${EditorPngHeaderHash};${EditorZlibFinderHash};${CMAKE_C_FLAGS};${PROJECT_SOURCE_DIR}")
    set(EditorPngStamp "${PROJECT_BINARY_DIR}/extern/libpng/editor-configuration.txt")
    set(EditorPreviousPngConfiguration "")
    if(EXISTS "${EditorPngStamp}")
        file(READ "${EditorPngStamp}" EditorPreviousPngConfiguration)
    endif()
    if(NOT EditorPreviousPngConfiguration STREQUAL EditorPngConfiguration)
        file(REMOVE
            "${PROJECT_BINARY_DIR}/extern/libpng/pnglibconf.out"
            "${PROJECT_BINARY_DIR}/extern/libpng/pnglibconf.h"
            "${PROJECT_BINARY_DIR}/extern/libpng/scripts/intprefix.out")
        file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/extern/libpng")
        file(WRITE "${EditorPngStamp}" "${EditorPngConfiguration}")
    endif()
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/libpng" "${PROJECT_BINARY_DIR}/extern/libpng" EXCLUDE_FROM_ALL)

    if(NOT TARGET png_static)
        message(FATAL_ERROR "extern/libpng did not provide the png_static target")
    endif()
    # libpng only sets directory-scope include paths. Publish them on the target
    # because FreeType compiles pngshim.c, which includes <png.h>, and png.h
    # includes the pnglibconf.h generated into libpng's binary directory.
    target_include_directories(png_static PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extern/libpng>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/extern/libpng>")

    # FreeType uses the vendored zlib and libpng; both are required, so a broken
    # mapping fails configuration instead of silently dropping PNG glyphs.
    set(FT_DISABLE_ZLIB OFF CACHE BOOL "Use the zlib built from extern/zlib" FORCE)
    set(FT_REQUIRE_ZLIB ON CACHE BOOL "Require the vendored zlib" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "Do not require external bzip2" FORCE)
    set(FT_DISABLE_PNG OFF CACHE BOOL "Enable FreeType color bitmap glyphs" FORCE)
    set(FT_REQUIRE_PNG ON CACHE BOOL "Require the vendored libpng" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "Disable FreeType WOFF2" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "Avoid FreeType/HarfBuzz dependency cycle" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/freetype" "${PROJECT_BINARY_DIR}/extern/freetype" EXCLUDE_FROM_ALL)

    set(HB_HAVE_FREETYPE ON CACHE BOOL "Enable FreeType integration" FORCE)
    foreach(Feature IN ITEMS ICU GLIB GOBJECT INTROSPECTION CAIRO GRAPHITE2 CORETEXT UNISCRIBE GDI DIRECTWRITE)
        set(HB_HAVE_${Feature} OFF CACHE BOOL "Disable optional HarfBuzz ${Feature}" FORCE)
    endforeach()
    foreach(Component IN ITEMS UTILS SUBSET RASTER VECTOR GPU)
        set(HB_BUILD_${Component} OFF CACHE BOOL "Disable unused HarfBuzz ${Component}" FORCE)
    endforeach()
    set(HB_BUILD_GPU_DEMO OFF CACHE STRING "Disable HarfBuzz GPU demo" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/harfbuzz" "${PROJECT_BINARY_DIR}/extern/harfbuzz" EXCLUDE_FROM_ALL)

    # Header-only dependency: do not configure upstream benchmarks or tests.
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/earcut" "${PROJECT_BINARY_DIR}/extern/earcut" EXCLUDE_FROM_ALL)
endfunction()
