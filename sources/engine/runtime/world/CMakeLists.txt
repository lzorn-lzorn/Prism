add_internal_library(World
    TYPE               INTERFACE
    INCLUDES           "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    PUBLIC_DEPENDENCY  Infra::Infra
    ALIAS              Runtime::World
)

add_library(Runtime_World INTERFACE)
target_link_libraries(Runtime_World INTERFACE Runtime::World)