add_subdirectory(material)
add_subdirectory(rdg)
add_subdirectory(renderer)

add_library(Renderer INTERFACE)
add_library(Runtime::Renderer ALIAS Renderer)

target_link_libraries(Renderer INTERFACE
    Runtime::RendererMaterial
    Runtime::RendererRDG
    Runtime::RendererCore
)

add_library(Runtime_Renderer INTERFACE)
target_link_libraries(Runtime_Renderer INTERFACE Renderer)
