cmake_minimum_required(VERSION 3.24)
project(Earcut LANGUAGES CXX)

set(EarcutIncludeDirectory "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${EarcutIncludeDirectory}/mapbox")
configure_file(earcut.hpp "${EarcutIncludeDirectory}/mapbox/earcut.hpp" COPYONLY)

add_library(EditorEarcut INTERFACE)
add_library(mapbox::earcut ALIAS EditorEarcut)
target_compile_features(EditorEarcut INTERFACE cxx_std_11)
target_include_directories(EditorEarcut SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${EarcutIncludeDirectory}>")