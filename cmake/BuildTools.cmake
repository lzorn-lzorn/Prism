include_guard(GLOBAL)

# 针对 XCode 提供的用户头文件搜索路径设置函数
function(user_header_search_paths target includes)
  if(XCODE)
    set(INCLUDES "")
    foreach(INC ${includes})
      get_filename_component(Absolute_Include_Path ${INC} ABSOLUTE)
      string(APPEND INCLUDES " \"${Absolute_Include_Path}\"")
    endforeach()
    set_target_properties(${target} PROPERTIES
      XCODE_ATTRIBUTE_USER_HEADER_SEARCH_PATHS "${INCLUDES}"
    )
  endif()
endfunction()

function(set_target_folder target)
  if(IDE_GROUP_PROJECTS_IN_FOLDERS)
    get_filename_component(FolderDir ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
    string(REPLACE ${CMAKE_SOURCE_DIR} "" FolderDir ${FolderDir})
    if(NOT FolderDir STREQUAL "")
      # 去掉开头的路径分隔符
      string(REGEX REPLACE "^[/\\]+" "" FolderDir ${FolderDir})
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "${FolderDir}")
  endif()
endfunction()

# ! 以下方式添加外部库的方式存疑, 原本希望解决的问题是将一些编译比较慢的库仅编译一次, 
# ! 放在 lib/<对应平台>/ 下, 之后不管是重新编译还是删除 build 目录都不用再次编译外部库
# ! 如果是:
# ! ```cmake
# if (TARGET DXC::Compiler)
#     target_link_libraries(${LibarayName}
#         PRIVATE
#             DXC::Compiler
#     )
# else()
#     add_subdirectory(DirectXShaderCompiler)
# endif()
# ! ```
# ! 这种的方式最后生成的库还是在 build 下.
# ! 但是以下实现的问题是最后输出的 dll 和 exe 不在一个目录下, 导致 exe 运行时找不到 dll
# ! 可能这种方式就不 work
# @brief 
# 	添加外部库的统一方式:
#   add_external_module(
#       NAME                <对外目标名>        # 最终链接时使用的目标名（支持命名空间，如 SDL3::SDL3）
#       SOURCE_DIR          <源码根目录>        # 第三方库源码路径（相对于项目根或绝对路径）
#       TARGET_IF_BUILT     <第三方CMake目标>   # 源码编译时，第三方库自身定义的 CMake 目标名
#       INCLUDE_DIR         <头文件目录>        # 头文件路径，对预编译和源码编译均生效
#       BUILD_SHARED_LIBS   <ON|OFF>           # ON：动态库  OFF：静态库
#       [CMAKE_ARGS         <参数...>]         # 可选，传递给第三方库 CMake 的变量（如 -DSDL_DISABLE_JOYSTICK=ON）
#   )
#
# @example
#   add_external_module(
#       NAME                SDL3::SDL3
#       SOURCE_DIR          extern/SDL
#       TARGET_IF_BUILT     SDL3::SDL3
#       INCLUDE_DIR         extern/SDL/include
#       BUILD_SHARED_LIBS   ON
#       CMAKE_ARGS          SDL_DISABLE_JOYSTICK=ON
#   )
# @note 
# 	该函数会在先在 lib/{对应平台}/ 下寻找是否有对应的库, 有则直接包一层 Interface 目标,
# 	没有则从源码编译并生成 manifest.cmake 再次包一层 Interface 目标.
function(add_external_module)
    set(oneValueArgs NAME SOURCE_DIR TARGET_IF_BUILT INCLUDE_DIR BUILD_SHARED_LIBS)
    set(multiValueArgs CMAKE_ARGS)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # ----- 平台判定 -----
    if(WIN32)
        set(platform windows)
    elseif(APPLE)
        set(platform macos)
    else()
        set(platform linux)
    endif()

    # 确保 NAME 是合法的目录名（不能包含 '::'）
    string(REPLACE "::" "_" safe_name "${ARG_NAME}")
    set(lib_root   "${CMAKE_SOURCE_DIR}/lib/${platform}")
    set(lib_dir    "${lib_root}/${safe_name}")
    set(manifest   "${lib_dir}/manifest.cmake")

    # ----- 1. 尝试读取预编译清单 -----
    set(use_prebuilt FALSE)
    if(EXISTS ${manifest})
        include(${manifest})   # 期望获得 PREBUILT_VALID 等变量
        if(PREBUILT_VALID)
            set(use_prebuilt TRUE)
            message(STATUS "[${ARG_NAME}] Using prebuilt library from ${lib_dir}")
        endif()
    endif()

    # ----- 2a. 使用预编译库 -----
    if(use_prebuilt)
        add_library(${ARG_NAME} ${PREBUILT_TYPE} IMPORTED)
        target_include_directories(${ARG_NAME} INTERFACE ${ARG_INCLUDE_DIR})

        if(PREBUILT_TYPE STREQUAL "SHARED" AND WIN32)
            set_target_properties(${ARG_NAME} PROPERTIES
                IMPORTED_LOCATION "${PREBUILT_IMPORTED_LOCATION}"
                IMPORTED_IMPLIB   "${PREBUILT_IMPORTED_IMPLIB}"
            )
        else()
            set_target_properties(${ARG_NAME} PROPERTIES
                IMPORTED_LOCATION "${PREBUILT_IMPORTED_LOCATION}"
            )
        endif()

    # ----- 2b. 从源码构建 + 自动生成清单 -----
    else()
        message(STATUS "[${ARG_NAME}] Building from source: ${ARG_SOURCE_DIR}")

        if(NOT TARGET ${ARG_TARGET_IF_BUILT})
            set(BUILD_SHARED_LIBS ${ARG_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
            add_subdirectory(${ARG_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/${safe_name}_build)
        endif()

        set(internal_target ${ARG_TARGET_IF_BUILT})
        if(TARGET ${internal_target})
            # 将输出重定向到 lib/<平台>/<安全名>/
            set_target_properties(${internal_target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${lib_dir}"
                ARCHIVE_OUTPUT_DIRECTORY "${lib_dir}"
                LIBRARY_OUTPUT_DIRECTORY "${lib_dir}"
            )

            # 确定外部生成脚本的位置（与本函数文件同目录）
            set(gen_manifest_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateManifest.cmake")
            if(NOT EXISTS "${gen_manifest_script}")
                message(FATAL_ERROR "GenerateManifest.cmake not found at ${gen_manifest_script}")
            endif()

            add_custom_command(TARGET ${internal_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -P "${gen_manifest_script}"
                    -D "lib_dir=${lib_dir}"
                    -D "manifest=${manifest}"
                    -D "is_shared=${ARG_BUILD_SHARED_LIBS}"
                COMMENT "Generating manifest for ${ARG_NAME}"
            )
        endif()

        # 创建接口目标，对外提供统一名称
        add_library(${ARG_NAME} INTERFACE)
        target_link_libraries(${ARG_NAME} INTERFACE ${internal_target})
        target_include_directories(${ARG_NAME} INTERFACE ${ARG_INCLUDE_DIR})
    endif()
endfunction()

# 根据 REQUIRED 决定报错或警告
function(_prebuilt_error msg required)
    if(required)
        message(FATAL_ERROR "[ARG_NAME] ${msg} in ${ARG_LIB_DIR}")
    else()
        message(WARNING "[ARG_NAME] ${msg}, target not created")
    endif()
endfunction()

# 确保传入的 CMake 列表中没有重复项. 
function(list_assert_duplicates values)
  set(items ${values})
  set(unique_items ${items})
  list(REMOVE_DUPLICATES unique_items)

  list(LENGTH items item_count)
  list(LENGTH unique_items unique_item_count)
  if(NOT item_count EQUAL unique_item_count)
    message(FATAL_ERROR "Duplicate entries found in list: ${items}")
  endif()
endfunction()

# internal_source_group - 为指定目标创建与文件系统目录对应的 IDE 源文件树
#   使用 source_group(TREE ...) 自动将 SOURCES 按相对路径分组
#   通常以 CMAKE_CURRENT_SOURCE_DIR 为根目录，您也可以指定自定义根
function(internal_source_group target)
    # 获取目标的源文件列表（仅直接添加的，不含生成器表达式）
    get_target_property(sources ${target} SOURCES)
    if(NOT sources)
        return()
    endif()

    # 确定树根目录：默认为当前源目录，也可通过参数传入
    set(tree_root ${CMAKE_CURRENT_SOURCE_DIR})
    if(ARGC GREATER 1)
        set(tree_root ${ARGV1})
    endif()

    # 仅当树根是绝对路径时执行（避免相对路径问题）
    if(IS_ABSOLUTE ${tree_root})
        source_group(TREE ${tree_root} FILES ${sources})
    endif()
endfunction()


# @brief
#   add_internal_library(<name>
#       TYPE      <STATIC|SHARED|OBJECT|INTERFACE>   # 可选, 默认 STATIC
#       SOURCES   <src1> [<src2> ...]                # 源文件列表(接口库不填写)
#       INCLUDES  <inc1> [<inc2> ...]                # PUBLIC 头文件路径, 告诉调用者如何找到我的头文件
#       PRIVATE_INCLUDES   <inc1> [<inc2> ...]       # PRIVATE 头文件路径, 内部依赖了哪些头文件
#       PUBLIC_DEPENDENCY  <target1> [<target2> ...] # 对外传递的依赖 (一般情况不用传递自己的内部依赖)
#       PRIVATE_DEPENDENCY <target1> [<target2> ...] # 不对外传递的依赖
#       ALIAS              <alias_name>              # 可选, 创建别名目标
#   )
# @example
# add_internal_library(Renderer
#     TYPE                STATIC
#     SOURCES             src/renderer.cpp
#     INCLUDES            include/renderer        # 公开头文件
#     PRIVATE_INCLUDES    src/renderer/private    # 内部头文件
#     PUBLIC_DEPENDENCY   EngineCore              # 依赖 EngineCore，且对外传导
#     ALIAS               Renderer::Impl
# )
# 使用者：
#   target_link_libraries(MyApp PRIVATE Renderer::Impl)
#   自动获得 include/renderer 路径，也能间接获得 EngineCore 的头文件
function(add_internal_library name)
    set(options "")
    set(oneValueArgs TYPE ALIAS)
    set(multiValueArgs SOURCES INCLUDES PRIVATE_INCLUDES
                      PUBLIC_DEPENDENCY PRIVATE_DEPENDENCY)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 默认类型
    if(NOT ARG_TYPE)
        set(ARG_TYPE STATIC)
    endif()

    # 创建库
    if(ARG_TYPE STREQUAL "INTERFACE")
        if(ARG_SOURCES)
            message(FATAL_ERROR "add_internal_library: INTERFACE library ${name} cannot have SOURCES")
        endif()
        add_library(${name} INTERFACE)
    else()
        if(NOT ARG_SOURCES)
            message(FATAL_ERROR "add_internal_library: non-INTERFACE library ${name} requires SOURCES")
        endif()
        add_library(${name} ${ARG_TYPE} ${ARG_SOURCES})
    endif()

    # 别名
    if(ARG_ALIAS)
        if(TARGET ${ARG_ALIAS})
            message(FATAL_ERROR "add_internal_library: ALIAS ${ARG_ALIAS} already exists")
        endif()
        add_library(${ARG_ALIAS} ALIAS ${name})
    endif()

    # 用户自定义配置（尽量放在 include 设置之前，避免覆盖）
    if(COMMAND seed_configure_target)
        seed_configure_target(${name})
    endif()

    # ---------- 设置包含目录 ----------
    # 1. PUBLIC 头文件路径
    if(ARG_INCLUDES)
        if(ARG_TYPE STREQUAL "INTERFACE")
            target_include_directories(${name} INTERFACE ${ARG_INCLUDES})
        else()
            target_include_directories(${name} PUBLIC ${ARG_INCLUDES})
        endif()
    endif()

    # 2. PRIVATE 头文件路径（仅用于编译本目标的源文件）
    if(ARG_PRIVATE_INCLUDES)
        if(ARG_TYPE STREQUAL "INTERFACE")
            message(WARNING "add_internal_library: INTERFACE library ${name} cannot have PRIVATE_INCLUDES, ignored")
        else()
            # 转换为绝对路径（避免相对路径问题）
            set(abs_private_includes "")
            foreach(inc ${ARG_PRIVATE_INCLUDES})
                get_filename_component(abs_inc "${inc}" ABSOLUTE)
                list(APPEND abs_private_includes "${abs_inc}")
            endforeach()
            target_include_directories(${name} PRIVATE ${abs_private_includes})
            # 调试输出（可移除）
            message(STATUS "Module ${name} PRIVATE includes: ${abs_private_includes}")
        endif()
    endif()

    # ---------- 依赖 ----------
    if(ARG_PUBLIC_DEPENDENCY)
        if(ARG_TYPE STREQUAL "INTERFACE")
            target_link_libraries(${name} INTERFACE ${ARG_PUBLIC_DEPENDENCY})
        else()
            target_link_libraries(${name} PUBLIC ${ARG_PUBLIC_DEPENDENCY})
        endif()
    endif()

    if(ARG_PRIVATE_DEPENDENCY)
        if(ARG_TYPE STREQUAL "INTERFACE")
            message(FATAL_ERROR "add_internal_library: INTERFACE library ${name} cannot have PRIVATE_DEPENDENCY")
        endif()
        target_link_libraries(${name} PRIVATE ${ARG_PRIVATE_DEPENDENCY})
    endif()

    # 其他属性设置...
    if(NOT ARG_TYPE STREQUAL "OBJECT" AND NOT ARG_TYPE STREQUAL "INTERFACE")
        set_property(TARGET ${name} APPEND PROPERTY
                     LINK_INTERFACE_MULTIPLICITY 3)
    endif()

    set_target_folder(${name})

    if(NOT ARG_TYPE STREQUAL "INTERFACE")
        internal_source_group(${name})
        list_assert_duplicates("${ARG_SOURCES}")
    endif()

    if(ARG_INCLUDES)
        user_header_search_paths(${name} "${ARG_INCLUDES}")
        list_assert_duplicates("${ARG_INCLUDES}")
    endif()

    set_property(GLOBAL APPEND PROPERTY INTERNAL_MODULES_LIST ${name})
endfunction()

# 判断某个头文件目录是否命中 EXCLUDED_HEADER_DIRS 屏蔽列表.
#   - 支持按“目录名”(最后一级)匹配, 也支持按“相对模块根目录的路径”匹配;
#   - 会沿父目录逐级向上回溯, 因此屏蔽 "internal" 时, "internal/deep" 也会被屏蔽;
#   - 模块根目录本身不参与匹配(它是模块整体, 而不是可屏蔽的内部子目录).
function(_seed_module_dir_is_excluded result dir module_root exclude_dirs)
    file(TO_CMAKE_PATH "${dir}" _dir)
    file(TO_CMAKE_PATH "${module_root}" _root)

    set(_excluded FALSE)
    set(_cur "${_dir}")
    while(NOT _cur STREQUAL _root)
        file(RELATIVE_PATH _rel "${_root}" "${_cur}")
        get_filename_component(_base "${_cur}" NAME)

        foreach(_entry IN LISTS exclude_dirs)
            file(TO_CMAKE_PATH "${_entry}" _entry)
            string(STRIP "${_entry}" _entry)
            string(REGEX REPLACE "/+$" "" _entry "${_entry}")
            if(_entry STREQUAL "")
                continue()
            endif()
            if(_rel STREQUAL _entry OR _base STREQUAL _entry)
                set(_excluded TRUE)
                break()
            endif()
        endforeach()

        if(_excluded)
            break()
        endif()

        get_filename_component(_parent "${_cur}" DIRECTORY)
        if(_parent STREQUAL _cur)
            break()
        endif()
        set(_cur "${_parent}")
    endwhile()

    set(${result} ${_excluded} PARENT_SCOPE)
endfunction()


# @brief
#   按模块构建：不区分 includes 与 sources 的构建工具.
#   与 add_internal_library 行为基本一致, 区别在于无需手动指定 SOURCES / INCLUDES:
#     1. 自动递归收集当前 CMakeLists 目录下的 .h / .hpp / .cpp 文件;
#     2. 头文件所在目录默认作为 PUBLIC 头文件路径开放给外部,
#        通过 EXCLUDED_HEADER_DIRS 屏蔽指定目录(按目录名或相对路径匹配),
#        被屏蔽目录中的 .h / .hpp 仅作为 PRIVATE 头文件, 不对外暴露;
#     3. 模块根目录会作为 PRIVATE 头文件路径加入, 便于内部以模块根为基准 include.
#
#   add_internal_module(<name>
#       TYPE                  <STATIC|SHARED|OBJECT|INTERFACE>   # 可选, 默认 STATIC
#       PUBLIC_DEPENDENCY     <target1> [<target2> ...]          # 对外传递的依赖
#       PRIVATE_DEPENDENCY    <target1> [<target2> ...]          # 不对外传递的依赖
#       ALIAS                 <alias_name>                       # 可选, 创建别名目标
#       EXCLUDED_HEADER_DIRS  <dir1> [<dir2> ...]                # 可选, 屏蔽头文件目录列表(目录名或相对路径)
#   )
#
# @example
#   add_internal_module(World
#       PRIVATE_DEPENDENCY   Core::Core
#       ALIAS                Runtime::World
#       EXCLUDED_HEADER_DIRS internal detail
#   )
#   上述写法会:
#     - 把当前目录下所有 .cpp 作为 SOURCES;
#     - 把除 internal/ detail/ 之外的目录作为 PUBLIC 头文件路径;
#     - 把 internal/ detail/ 以及模块根目录作为 PRIVATE 头文件路径.
function(add_internal_module name)
    set(options "")
    set(oneValueArgs TYPE ALIAS)
    set(multiValueArgs PUBLIC_DEPENDENCY PRIVATE_DEPENDENCY EXCLUDED_HEADER_DIRS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # SOURCES / INCLUDES / PRIVATE_INCLUDES 均由本函数自动推导, 不允许手动传入.
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "add_internal_module: unexpected arguments '${ARG_UNPARSED_ARGUMENTS}'. SOURCES/INCLUDES/PRIVATE_INCLUDES 由本函数自动推导, 请勿手动传入.")
    endif()

    # 默认类型
    if(NOT ARG_TYPE)
        set(ARG_TYPE STATIC)
    endif()

    # ---------- 1. 自动收集源文件与头文件 ----------
    file(GLOB_RECURSE _module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    )
    file(GLOB_RECURSE _module_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    )

    if(ARG_TYPE STREQUAL "INTERFACE")
        if(_module_sources)
            message(FATAL_ERROR "add_internal_module: INTERFACE 模块 ${name} 不能包含 .cpp 文件, 请改用 STATIC 或移除实现文件: ${_module_sources}")
        endif()
    elseif(NOT _module_sources)
        message(FATAL_ERROR "add_internal_module: 在 ${CMAKE_CURRENT_SOURCE_DIR} 下未找到任何 .cpp 文件; 头文件模块请使用 TYPE INTERFACE")
    endif()

    # ---------- 2. 根据头文件位置推导 include 目录 ----------
    set(_public_includes "")
    set(_private_includes "")
    foreach(_header IN LISTS _module_headers)
        get_filename_component(_header_dir "${_header}" DIRECTORY)
        _seed_module_dir_is_excluded(_excluded "${_header_dir}" "${CMAKE_CURRENT_SOURCE_DIR}" "${ARG_EXCLUDED_HEADER_DIRS}")
        if(_excluded)
            list(APPEND _private_includes "${_header_dir}")
        else()
            list(APPEND _public_includes "${_header_dir}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES _public_includes)
    list(REMOVE_DUPLICATES _private_includes)

    # 模块根目录作为 PRIVATE include, 便于内部以模块根为基准 include (如 #include "internal/foo.hpp")
    if(NOT ARG_TYPE STREQUAL "INTERFACE")
        list(APPEND _private_includes "${CMAKE_CURRENT_SOURCE_DIR}")
        list(REMOVE_DUPLICATES _private_includes)
    endif()

    # INTERFACE 库不能携带 SOURCES 与 PRIVATE_INCLUDES
    if(ARG_TYPE STREQUAL "INTERFACE")
        set(_module_sources "")
        set(_module_headers "")
        set(_private_includes "")
    endif()

    # ---------- 3. 委托给 add_internal_library 复用统一逻辑 ----------
    add_internal_library(${name}
        TYPE                ${ARG_TYPE}
        SOURCES             ${_module_sources} ${_module_headers}
        INCLUDES            ${_public_includes}
        PRIVATE_INCLUDES    ${_private_includes}
        PUBLIC_DEPENDENCY   ${ARG_PUBLIC_DEPENDENCY}
        PRIVATE_DEPENDENCY  ${ARG_PRIVATE_DEPENDENCY}
        ALIAS               ${ARG_ALIAS}
    )
endfunction()

macro(remove_c_flag flag)

  foreach(f ${ARGV})
    string(REGEX REPLACE ${f} "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE ${f} "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE ${f} "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REGEX REPLACE ${f} "" CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL}")
    string(REGEX REPLACE ${f} "" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  endforeach()
  unset(f)
endmacro()

macro(remove_cxx_flag flag)
  foreach(f ${ARGV})
    string(REGEX REPLACE ${f} "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE ${f} "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE ${f} "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REGEX REPLACE ${f} "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")
    string(REGEX REPLACE ${f} "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  endforeach()
  unset(f)
endmacro()

macro(remove_c_and_cxx_flag flag)
  remove_c_flag(${ARGV})
  remove_cxx_flag(${ARGV})
endmacro()

macro(add_c_flag flag)

  string(APPEND CMAKE_C_FLAGS " ${flag}")
endmacro()

macro(add_cxx_flag flag)
  string(APPEND CMAKE_CXX_FLAGS " ${flag}")
endmacro()

macro(add_c_and_cxx_flag flag)
  add_c_flag("${flag}")
  add_cxx_flag("${flag}")
endmacro()

macro(add_c_flag_per_config flag)
  string(APPEND CMAKE_C_FLAGS_DEBUG " ${flag}")
  string(APPEND CMAKE_C_FLAGS_RELEASE " ${flag}")
  string(APPEND CMAKE_C_FLAGS_MINSIZEREL " ${flag}")
  string(APPEND CMAKE_C_FLAGS_RELWITHDEBINFO " ${flag}")
endmacro()

macro(add_cxx_flag_per_config flag)
  string(APPEND CMAKE_CXX_FLAGS_DEBUG " ${flag}")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " ${flag}")
  string(APPEND CMAKE_CXX_FLAGS_MINSIZEREL " ${flag}")
  string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " ${flag}")
endmacro()

macro(remove_strict_flags)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    remove_c_and_cxx_flag(
      "-Wstrict-prototypes"
      "-Wsuggest-attribute=format"
      "-Wmissing-prototypes"
      "-Wmissing-declarations"
      "-Wmissing-format-attribute"
      "-Wunused-local-typedefs"
      "-Wunused-macros"
      "-Wunused-parameter"
      "-Wwrite-strings"
      "-Wredundant-decls"
      "-Wundef"
      "-Wshadow"
      "-Wdouble-promotion"
      "-Wold-style-definition"
      "-Wextra"
      "-Werror=[^ ]+"
      "-Werror"
    )

    # negate flags implied by '-Wall'
    add_c_flag("${C_REMOVE_STRICT_FLAGS}")
    add_cxx_flag("${CXX_REMOVE_STRICT_FLAGS}")
    add_c_and_cxx_flag("${C_AND_CXX_REMOVE_STRICT_FLAGS}")
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    remove_c_and_cxx_flag(
      "-Wunused-parameter"
      "-Wunused-variable"
      "-Werror=[^ ]+"
      "-Werror"
    )

    # negate flags implied by '-Wall'
    add_c_flag("${C_REMOVE_STRICT_FLAGS}")
    add_cxx_flag("${CXX_REMOVE_STRICT_FLAGS}")
    add_c_and_cxx_flag("${C_AND_CXX_REMOVE_STRICT_FLAGS}")
  endif()

  if(MSVC)
    add_cxx_flag(
      # Warning C5038: data member 'foo' will be initialized after data member 'bar'.
      "/wd5038"
    )
    remove_c_and_cxx_flag(
      # Restore warn C4100 (unreferenced formal parameter) back to w4.
      "/w34100"
      # Restore warn C4189 (unused variable) back to w4.
      "/w34189"
    )
  endif()

endmacro()

macro(remove_extra_strict_flags)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    remove_c_and_cxx_flag(
      "-Wunused-parameter"
    )
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    remove_c_and_cxx_flag(
      "-Wunused-parameter"
    )
  endif()

  if(MSVC)
    remove_c_and_cxx_flag(
      # Restore warn C4100 (unreferenced formal parameter) back to w4.
      "/w34100"
    )
  endif()
endmacro()

function(remove_strict_c_flags_file filenames)
  foreach(SOURCE ${ARGV})
    if((CMAKE_C_COMPILER_ID STREQUAL "GNU") OR
       (CMAKE_C_COMPILER_ID MATCHES "Clang"))
      set_source_files_properties(
        ${SOURCE} PROPERTIES
        COMPILE_FLAGS "${C_REMOVE_STRICT_FLAGS} ${C_AND_CXX_REMOVE_STRICT_FLAGS}"
      )
    endif()
    if(MSVC)
    	set_source_files_properties(${SOURCE} PROPERTIES
			COMPILE_FLAGS "${MSVC_C_RELAX_FLAGS} ${MSVC_COMMON_RELAX_FLAGS}"
		)
    endif()
  endforeach()
endfunction()

function(remove_strict_cxx_flags_file filenames)
  foreach(SOURCE ${ARGV})
    if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR
       (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
      set_source_files_properties(
        ${SOURCE} PROPERTIES
        COMPILE_FLAGS "${CXX_REMOVE_STRICT_FLAGS} ${C_AND_CXX_REMOVE_STRICT_FLAGS}"
      )
    endif()
    if(MSVC)
    	set_source_files_properties(${SOURCE} PROPERTIES
			COMPILE_FLAGS "${MSVC_C_RELAX_FLAGS} ${MSVC_COMMON_RELAX_FLAGS}"
		)
    endif()
  endforeach()
endfunction()

# External libs may need 'signed char' to be default.
macro(remove_c_and_cxx_flag_unsigned_char)
  if((CMAKE_C_COMPILER_ID STREQUAL "GNU") OR
     (CMAKE_C_COMPILER_ID MATCHES "Clang") OR
     (CMAKE_C_COMPILER_ID STREQUAL "Intel"))
    remove_c_and_cxx_flag("-funsigned-char")
  elseif(MSVC)
    remove_c_and_cxx_flag("/J")
  else()
    message(WARNING
      "Compiler '${CMAKE_C_COMPILER_ID}' failed to disable 'unsigned char' flag."
      "Build files need updating."
    )
  endif()
endmacro()

function(add_check_c_compiler_flag_impl
  _CFLAGS
  _CACHE_VAR
  _FLAG
  )

  include(CheckCCompilerFlag)

  set(_is_new TRUE)
  if(DEFINED CACHE{${_CACHE_VAR}})
    set(_is_new FALSE)
  endif()

  check_c_compiler_flag("${_FLAG}" "${_CACHE_VAR}")
  if(${_CACHE_VAR})
    # message(STATUS "Using CFLAG: ${_FLAG}")
    set(${_CFLAGS} "${${_CFLAGS}} ${_FLAG}" PARENT_SCOPE)
  else()
    if(_is_new)
      message(STATUS "Unsupported CFLAG: ${_FLAG}")
    endif()
  endif()
endfunction()

function(add_check_cxx_compiler_flag_impl
  _CXXFLAGS
  _CACHE_VAR
  _FLAG
  )

  include(CheckCXXCompilerFlag)

  set(_is_new TRUE)
  if(DEFINED CACHE{${_CACHE_VAR}})
    set(_is_new FALSE)
  endif()

  check_cxx_compiler_flag("${_FLAG}" "${_CACHE_VAR}")
  if(${_CACHE_VAR})
    # message(STATUS "Using CXXFLAG: ${_FLAG}")
    set(${_CXXFLAGS} "${${_CXXFLAGS}} ${_FLAG}" PARENT_SCOPE)
  else()
    if(_is_new)
      message(STATUS "Unsupported CXXFLAG: ${_FLAG}")
    endif()
  endif()
endfunction()

function(ADD_CHECK_C_COMPILER_FLAGS _CFLAGS)
  # Iterate over pairs & check each.
  set(cache_var "")
  foreach(arg ${ARGN})
    if(cache_var)
      add_check_c_compiler_flag_impl("${_CFLAGS}" "${cache_var}" "${arg}")
      set(cache_var "")
    else()
      set(cache_var "${arg}")
    endif()
  endforeach()
  set(${_CFLAGS} "${${_CFLAGS}}" PARENT_SCOPE)
endfunction()

function(ADD_CHECK_CXX_COMPILER_FLAGS _CXXFLAGS)
  # Iterate over pairs & check each.
  set(cache_var "")
  foreach(arg ${ARGN})
    if(cache_var)
      add_check_cxx_compiler_flag_impl("${_CXXFLAGS}" "${cache_var}" "${arg}")
      set(cache_var "")
    else()
      set(cache_var "${arg}")
    endif()
  endforeach()
  set(${_CXXFLAGS} "${${_CXXFLAGS}}" PARENT_SCOPE)
endfunction()


function(SeedCopyResources tgt)
    set(res_root ${CMAKE_BINARY_DIR}/resources)
    foreach(res assets shaders configs)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/${res} ${res_root}/${res}
            COMMENT "Syncing ${res} -> ${res_root}")
    endforeach()
endfunction()