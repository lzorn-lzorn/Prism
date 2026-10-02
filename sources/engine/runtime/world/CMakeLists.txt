add_internal_module(World
    TYPE               INTERFACE
    PUBLIC_DEPENDENCY  Core::Core
    ALIAS              Runtime::World
)

add_library(Runtime_World INTERFACE)
target_link_libraries(Runtime_World INTERFACE Runtime::World)
