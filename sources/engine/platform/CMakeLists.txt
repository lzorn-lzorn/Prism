# 平台抽象层: 引擎级窗口/事件/平台工厂(原 launcher/generic, 上移至引擎层).
#
# 依赖方向: Platform 仅依赖 Infra(基础) 与 SDL3(SDL 后端实现私有依赖);
# UI / Renderer / RHI / Engine 都依赖本模块, 反向不复存在.
add_internal_module(Platform
    PUBLIC_DEPENDENCY   Infra::Infra
    PRIVATE_DEPENDENCY  SDL3::SDL3
    ALIAS               Runtime::Platform
    EXCLUDED_HEADER_DIRS sdl
)
