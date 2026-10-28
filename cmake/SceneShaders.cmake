include_guard(GLOBAL)

# 编译场景(PBR 网格 / 后处理 / GPU 剔除)着色器为 SPIR-V, 供渲染器在运行时加载.
# 与 UIShaders.cmake 的 UI 着色器编译保持同一套 DXC 工具链约定.
#
# 用法: 在 renderer 目标 CMakeLists 里调用
#     include(${CMAKE_SOURCE_DIR}/cmake/SceneShaders.cmake)
#     seed_configure_scene_shaders(RendererCore)
# 之后 C++ 通过宏 SEED_SCENE_SHADER_DIR 定位编译产物并读取 .spv 文件.

function(seed_configure_scene_shaders Target)
    if(NOT TARGET "${Target}")
        message(FATAL_ERROR "seed_configure_scene_shaders requires an existing target")
    endif()
    if(TARGET "${Target}_scene_shaders")
        return()
    endif()

    find_program(DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

    set(SourceDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../shaders")
    set(ShaderDir "${CMAKE_CURRENT_BINARY_DIR}/generated-scene-shaders")
    set(SPIRVFlags -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout)

    # (源文件, 入口, profile, 输出名)
    set(SceneShaders
        "scene_mesh.vert.hlsl main vs_6_0 scene_mesh.vert.spv"
        "scene_mesh.frag.hlsl main ps_6_0 scene_mesh.frag.spv"
        "fullscreen.vert.hlsl main vs_6_0 fullscreen.vert.spv"
        "post.frag.hlsl       main ps_6_0 post.frag.spv"
        "cull.comp.hlsl       main cs_6_0 cull.comp.spv"
    )

    set(Outputs)
    foreach(Entry IN LISTS SceneShaders)
        separate_arguments(Args NATIVE_COMMAND "${Entry}")
        list(GET Args 0 SourceName)
        list(GET Args 1 EntryPoint)
        list(GET Args 2 Profile)
        list(GET Args 3 OutputName)
        set(Source "${SourceDir}/${SourceName}")
        set(Output "${ShaderDir}/${OutputName}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Source}")
        add_custom_command(
            OUTPUT "${Output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${ShaderDir}"
            COMMAND "${DXC}" ${SPIRVFlags} -E "${EntryPoint}" -T "${Profile}"
                "${Source}" -Fo "${Output}"
            DEPENDS "${Source}" "${DXC}"
            VERBATIM)
        list(APPEND Outputs "${Output}")
    endforeach()

    add_custom_target("${Target}_scene_shaders" DEPENDS ${Outputs})
    add_dependencies("${Target}" "${Target}_scene_shaders")
    target_compile_definitions("${Target}" PRIVATE
        SEED_SCENE_SHADER_DIR="${ShaderDir}")
    message(STATUS "Scene shaders: SPIR-V via ${DXC} -> ${ShaderDir}")
endfunction()
