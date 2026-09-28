file(GLOB_RECURSE SOURCES_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/includes/*.hpp
)

add_internal_library(GenericApplication
    TYPE                STATIC
    SOURCES             ${SOURCES_LIST}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    PRIVATE_INCLUDES    "${CMAKE_CURRENT_SOURCE_DIR}/sources"
                        "${CMAKE_CURRENT_SOURCE_DIR}/includes/generic_application"
    PRIVATE_DEPENDENCY  Core::Core
    ALIAS               Module::GenericApplication
)

if(USE_SDL)
    target_link_libraries(GenericApplication PRIVATE SDL3::SDL3)
endif()