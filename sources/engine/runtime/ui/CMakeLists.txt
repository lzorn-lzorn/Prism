add_internal_module(UI
    PUBLIC_DEPENDENCY   Infra::Infra Generic::Generic
    PRIVATE_DEPENDENCY  freetype harfbuzz EditorEarcut
    ALIAS               UI::UI
)
if(WIN32)
    target_link_libraries(UI PRIVATE dwrite)
endif()