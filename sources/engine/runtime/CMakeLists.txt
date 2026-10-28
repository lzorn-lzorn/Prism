
# 设置公共头文件目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB Children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT Children)

# 模块之间存在依赖, 而 target_link_libraries 对带 `::` 的目标不做延迟解析
# (要求目标在调用时已存在), 因此必须按拓扑序配置, 否则会因前向引用而失败:
#   engine       -> Runtime::World / Runtime::Renderer / Runtime::Logger / Runtime::Camera
#   renderer     -> Runtime::World / UI::UI (RendererCore 还依赖自身的 material/rdg 子模块)
#   asset_manager-> UI::UI (loader)
# 依赖序: 叶子模块(world/camera/logger) -> ui -> asset_manager -> renderer -> engine.
set(_SeedRuntimeOrder
    world
    camera
    logger
    ui
    asset_manager
    renderer
    engine
)
set(_SeedRuntimeOrdered "")
foreach(_Mod IN LISTS _SeedRuntimeOrder)
    if(_Mod IN_LIST Children)
        list(APPEND _SeedRuntimeOrdered ${_Mod})
    endif()
endforeach()
# 未来新增的子目录不在上述清单时, 追加到末尾(它们若引入新的前向依赖, 需在此补充排序).
foreach(_Mod IN LISTS Children)
    if(NOT _Mod IN_LIST _SeedRuntimeOrdered)
        list(APPEND _SeedRuntimeOrdered ${_Mod})
    endif()
endforeach()
set(Children ${_SeedRuntimeOrdered})

foreach(Child ${Children})
	if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${Child} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${Child}/CMakeLists.txt)
		add_subdirectory(${Child})
		add_library(Runtime_${Child} INTERFACE)
		
	endif()
endforeach()

add_library(Runtime INTERFACE)
# 收集所有内部真实目标名（由 add_internal_library 添加到全局属性）
get_property(RuntimeModules GLOBAL PROPERTY INTERNAL_MODULES_LIST)
if (RuntimeModules)
	list(REMOVE_DUPLICATES RuntimeModules)
    target_link_libraries(Runtime INTERFACE ${RuntimeModules})
	message(STATUS "Runtime modules: ${RuntimeModules}")
endif()

add_library(Runtime::Runtime ALIAS Runtime)
target_include_directories(Runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})