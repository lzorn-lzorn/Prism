add_internal_module(RendererMaterial
    TYPE               STATIC
    PUBLIC_DEPENDENCY  Module::RHI
    PRIVATE_DEPENDENCY Core::Core
    ALIAS              Runtime::RendererMaterial
)

# Keep canonical include style: <material/Material.hpp>
target_include_directories(RendererMaterial PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
