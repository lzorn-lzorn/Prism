include_guard(GLOBAL)

function(seed_configure_ui_shaders Target)
    if(NOT TARGET "${Target}")
        message(FATAL_ERROR "seed_configure_ui_shaders requires an existing target")
    endif()
    if(TARGET "${Target}_ui_shaders")
        return()
    endif()

    find_program(DXC NAMES dxc REQUIRED)
    option(EDITOR_UI_BUILD_MSL "Generate Metal shaders (required for macOS)" ${APPLE})
    option(EDITOR_UI_BUILD_DXIL "Generate Direct3D12 shaders (required for D3D12)" ${WIN32})
    if(EDITOR_UI_BUILD_MSL)
        find_program(SPIRV_CROSS NAMES spirv-cross REQUIRED)
    endif()

    set(SourceDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../shaders")
    set(ShaderDir "${CMAKE_CURRENT_BINARY_DIR}/generated-ui-shaders")
    set(SPIRVFlags -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout)
    set(ProbeDir "${ShaderDir}/toolchain-probe")
    file(MAKE_DIRECTORY "${ProbeDir}")

    set(Outputs)
    foreach(Stage IN ITEMS vert frag)
        if(Stage STREQUAL "vert")
            set(Profile vs_6_0)
        else()
            set(Profile ps_6_0)
        endif()
        set(Source "${SourceDir}/ui.${Stage}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Source}")
        execute_process(
            COMMAND "${DXC}" ${SPIRVFlags} -E main -T "${Profile}" "${Source}"
                -Fo "${ProbeDir}/ui.${Stage}.spv"
            RESULT_VARIABLE CompileResult OUTPUT_QUIET ERROR_VARIABLE ProbeError)
        if(NOT CompileResult STREQUAL "0")
            message(FATAL_ERROR "UI ${Stage} SPIR-V toolchain probe failed: ${ProbeError}")
        endif()
        if(EDITOR_UI_BUILD_MSL)
        execute_process(
            COMMAND "${SPIRV_CROSS}" "${ProbeDir}/ui.${Stage}.spv"
                --msl --output "${ProbeDir}/ui.${Stage}.msl"
            RESULT_VARIABLE CompileResult OUTPUT_QUIET ERROR_VARIABLE ProbeError)
        if(NOT CompileResult STREQUAL "0")
            message(FATAL_ERROR "UI ${Stage} MSL toolchain probe failed: ${ProbeError}")
        endif()
        endif()
        if(EDITOR_UI_BUILD_DXIL)
        execute_process(
            COMMAND "${DXC}" -E main -T "${Profile}" "${Source}"
                -Fo "${ProbeDir}/ui.${Stage}.dxil"
            RESULT_VARIABLE CompileResult OUTPUT_QUIET ERROR_VARIABLE ProbeError)
        if(NOT CompileResult STREQUAL "0")
            message(FATAL_ERROR "UI ${Stage} DXIL toolchain probe failed: ${ProbeError}")
        endif()
        endif()
        set(Extensions spv)
        if(EDITOR_UI_BUILD_MSL)
            list(APPEND Extensions msl)
        endif()
        if(EDITOR_UI_BUILD_DXIL)
            list(APPEND Extensions dxil)
        endif()
        foreach(Extension IN LISTS Extensions)
            if(NOT EXISTS "${ProbeDir}/ui.${Stage}.${Extension}")
                message(FATAL_ERROR "UI toolchain probe did not produce ui.${Stage}.${Extension}")
            endif()
        endforeach()

        set(SPIRV "${ShaderDir}/ui.${Stage}.spv")
        set(MSL "${ShaderDir}/ui.${Stage}.msl")
        set(DXIL "${ShaderDir}/ui.${Stage}.dxil")
        set(StageOutputs "${SPIRV}")
        set(ExtraCommands)
        set(ToolDependencies "${DXC}")
        if(EDITOR_UI_BUILD_MSL)
            list(APPEND StageOutputs "${MSL}")
            list(APPEND ToolDependencies "${SPIRV_CROSS}")
            list(APPEND ExtraCommands COMMAND "${SPIRV_CROSS}" "${SPIRV}" --msl --output "${MSL}")
        endif()
        if(EDITOR_UI_BUILD_DXIL)
            list(APPEND StageOutputs "${DXIL}")
            list(APPEND ExtraCommands COMMAND "${DXC}" -E main -T "${Profile}" "${Source}" -Fo "${DXIL}")
        endif()
        add_custom_command(
            OUTPUT ${StageOutputs}
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${ShaderDir}"
            COMMAND "${DXC}" ${SPIRVFlags} -E main -T "${Profile}" "${Source}" -Fo "${SPIRV}"
            ${ExtraCommands}
            DEPENDS "${Source}" ${ToolDependencies}
            VERBATIM)
        list(APPEND Outputs ${StageOutputs})
    endforeach()

    add_custom_target("${Target}_ui_shaders" DEPENDS ${Outputs})
    add_dependencies("${Target}" "${Target}_ui_shaders")
    target_compile_features("${Target}" PRIVATE cxx_std_23)
    target_compile_definitions("${Target}" PRIVATE
        EDITOR_UI_SHADER_DIR="${ShaderDir}")
    if(EDITOR_UI_BUILD_DXIL)
        target_compile_definitions("${Target}" PRIVATE EDITOR_UI_HAS_DXIL=1)
    endif()
    if(EDITOR_UI_BUILD_MSL)
        target_compile_definitions("${Target}" PRIVATE EDITOR_UI_HAS_MSL=1)
    endif()
    message(STATUS "UI shaders: SPIR-V; MSL=${EDITOR_UI_BUILD_MSL}; DXIL=${EDITOR_UI_BUILD_DXIL}")
endfunction()