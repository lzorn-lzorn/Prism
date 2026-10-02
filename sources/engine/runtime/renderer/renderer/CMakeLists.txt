add_internal_module(RendererCore
    TYPE                STATIC
    PUBLIC_DEPENDENCY   Module::RHI Runtime::World Runtime::RendererRDG Runtime::RendererMaterial
    PRIVATE_DEPENDENCY  Core::Core spirv-reflect-static
    ALIAS               Runtime::RendererCore
)

# Keep canonical include style: <renderer/RendererServer.hpp>
target_include_directories(RendererCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_compile_definitions(RendererCore PRIVATE
    SEED_SPIRV_REFLECT_SAMPLE_PATH="${PROJECT_SOURCE_DIR}/extern/SPIRV-Reflect/examples/sample.spv"
)
