file(GLOB_RECURSE EditorSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/command_line/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/command_line/*.hpp"
)
add_internal_library(Editor
    SOURCES             ${EditorSources}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/app" "${CMAKE_CURRENT_SOURCE_DIR}/command_line"
    PUBLIC_DEPENDENCY   Runtime::Platform UI::UI Renderer::UIBackend Runtime::Renderer Runtime::AssetManagerLoader Runtime::Logger CLI11::CLI11 Runtime_Engine
    ALIAS               Editor::Editor
)