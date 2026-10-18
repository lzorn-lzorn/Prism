add_internal_module(AssetManagerLoader
    PUBLIC_DEPENDENCY   UI::UI
    PRIVATE_DEPENDENCY  Infra::Infra
    ALIAS               Runtime::AssetManagerLoader
)
target_include_directories(AssetManagerLoader SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/extern/stb_image)