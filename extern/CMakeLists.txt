
# 实际上很多库是无法使用和项目一样的编译选项的, 尤其是一些比较老的库, 使用最严格的编译检
# 查选项会导致编译失败, 所以这里提供了一个宏来移除一些编译选项, 以便在编译外部库时使用.
# 见 BuildTools.cmake

# TEST add_external_module
# add_external_module(
# 	NAME SDL3::SDL3
# 	SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/SDL"
# 	TARGET_IF_BUILT SDL3::SDL3
# 	INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/SDL/include"
# 	BUILD_SHARED_LIBS ON
# )

add_subdirectory(SDL)
# add_subdirectory(json) 
# add_subdirectory(tomlplusplus)

set(SPIRV_REFLECT_EXECUTABLE OFF) # 禁止构建 SPIRV-Reflect 的 examples
set(SPIRV_REFLECT_STATIC_LIB ON)  # Renderer 私有使用反射实现
add_subdirectory(SPIRV-Reflect)
# 这里 SPIRV-Reflect 的 examples 有一个 common.cpp 其会导致 clangd 和项目
# 里的 core/common/Common.hpp 错误被 clangd 配对用于静态分析, 但是项目本身
# 是基于 C++23 的, 而 SPIRV-Reflect 的 common.cpp 是基于 C++14 的, 所以 
# clangd 在 Common.hpp 中会按照 C++14 来分析内部所有的头文件, 进而报错


if (TARGET DXC::Compiler)
    target_link_libraries(${LibarayName}
        PRIVATE
            DXC::Compiler
    )
else()
    # add_subdirectory(DirectXShaderCompiler)
endif()

remove_strict_flags()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  remove_c_and_cxx_flag(
    "-Wextra"
    "-Wmisleading-indentation"
  )
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  remove_c_and_cxx_flag(
    "-Weverything"
    "-Wextra"
    "-Wnull-pointer-subtraction"
    "-Wcast-function-type-mismatch"
    "-Wignored-qualifiers"
  )
endif()