add_internal_module(RendererCore
    TYPE                STATIC
    PUBLIC_DEPENDENCY   Module::RHI Runtime::World Runtime::RendererRDG Runtime::RendererMaterial UI::UI
    PRIVATE_DEPENDENCY  Infra::Infra spirv-reflect-static
    ALIAS               Runtime::RendererCore
)

# Keep canonical include style: <renderer/RendererServer.hpp>
target_include_directories(RendererCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_compile_definitions(RendererCore PRIVATE
    SEED_SPIRV_REFLECT_SAMPLE_PATH="${PROJECT_SOURCE_DIR}/extern/SPIRV-Reflect/examples/sample.spv"
)

# 场景着色器(PBR 网格 / 后处理 / GPU 剔除)经 DXC 编译为 SPIR-V, 运行时加载.
include(${PROJECT_SOURCE_DIR}/cmake/SceneShaders.cmake)
seed_configure_scene_shaders(RendererCore)
