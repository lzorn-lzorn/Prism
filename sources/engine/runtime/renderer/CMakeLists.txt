file(GLOB_RECURSE SOURCES_LIST CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/includes/*.hpp
)

add_internal_library(Renderer
    TYPE                STATIC
    SOURCES             ${SOURCES_LIST}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
						"${CMAKE_CURRENT_SOURCE_DIR}/includes/renderer"
                        "${CMAKE_CURRENT_SOURCE_DIR}/includes/material"
    PRIVATE_INCLUDES    "${CMAKE_CURRENT_SOURCE_DIR}/sources"
    PUBLIC_DEPENDENCY   Module::RHI
    PRIVATE_DEPENDENCY  Core::Core spirv-reflect-static
    ALIAS               Runtime::Renderer
)
target_compile_definitions(Renderer PRIVATE
    SEED_SPIRV_REFLECT_SAMPLE_PATH="${PROJECT_SOURCE_DIR}/extern/SPIRV-Reflect/examples/sample.spv"
)
add_library(Runtime_Renderer INTERFACE)
target_link_libraries(Runtime_Renderer INTERFACE Renderer)
