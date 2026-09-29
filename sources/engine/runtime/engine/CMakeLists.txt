file(GLOB_RECURSE SOURCES_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/includes/*.hpp
)

add_internal_library(Engine
    TYPE                STATIC
    SOURCES             ${SOURCES_LIST}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
						"${CMAKE_CURRENT_SOURCE_DIR}/includes/engine"
    PRIVATE_INCLUDES    "${CMAKE_CURRENT_SOURCE_DIR}/sources"
    PRIVATE_DEPENDENCY  Core::Core 
						Runtime::Camera
						Runtime::Renderer
						Runtime::Logger
)

add_library(Runtime_Engine INTERFACE)
target_link_libraries(Runtime_Engine INTERFACE Engine)