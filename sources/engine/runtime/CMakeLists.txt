
# 设置公共头文件目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB Children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)

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