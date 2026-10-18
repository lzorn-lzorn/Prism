add_internal_module(UIBackend
    PUBLIC_DEPENDENCY   UI::UI Generic::Generic
    PRIVATE_DEPENDENCY  SDL3::SDL3
    ALIAS               Renderer::UIBackend
)
include(${CMAKE_SOURCE_DIR}/cmake/UIShaders.cmake)
seed_configure_ui_shaders(UIBackend)
target_include_directories(UIBackend PRIVATE ${CMAKE_SOURCE_DIR}/sources/engine/launcher/generic/sdl)