include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)
seed_add_source_dependencies()
seed_add_cli11()

add_subdirectory(infra)
add_subdirectory(launcher)
add_subdirectory(modules)
add_subdirectory(runtime)