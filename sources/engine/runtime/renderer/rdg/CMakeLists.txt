add_internal_module(RendererRDG
    TYPE               STATIC
    PUBLIC_DEPENDENCY  Module::RHI
    PRIVATE_DEPENDENCY Core::Core
    ALIAS              Runtime::RendererRDG
)

# Keep canonical include style: <rdg/RDGBuilder.hpp>
target_include_directories(RendererRDG PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
