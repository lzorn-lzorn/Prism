add_internal_module(Generic
    PUBLIC_DEPENDENCY   Infra::Infra
    PRIVATE_DEPENDENCY  SDL3::SDL3
    ALIAS               Generic::Generic
    EXCLUDED_HEADER_DIRS sdl
)