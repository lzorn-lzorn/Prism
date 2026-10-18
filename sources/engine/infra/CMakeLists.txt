add_library(Common STATIC
	sources/common/Error.cpp
	sources/common/Assert.cpp
	sources/common/GId.cpp
)
target_include_directories(Common PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_compile_features(Common PUBLIC cxx_std_23)
if(WIN32)
	target_link_libraries(Common PRIVATE ole32)
endif()

add_library(Container INTERFACE)
target_include_directories(Container INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_link_libraries(Container INTERFACE Common)

add_library(Wrappers INTERFACE)
target_include_directories(Wrappers INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_link_libraries(Wrappers INTERFACE Common)

add_library(Reflect INTERFACE)
target_include_directories(Reflect INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_link_libraries(Reflect INTERFACE Common)

add_library(Serde INTERFACE)
target_include_directories(Serde INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_link_libraries(Serde INTERFACE Reflect)

file(GLOB_RECURSE MathSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sources/math/*.cpp"
)
add_library(Math OBJECT ${MathSources})
target_include_directories(Math PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
target_link_libraries(Math PUBLIC Common)

file(GLOB_RECURSE MemorySources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sources/memory/*.cpp"
)
add_library(Memory OBJECT ${MemorySources})
target_include_directories(Memory PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
	$<INSTALL_INTERFACE:includes>
)
find_package(Threads REQUIRED)
target_link_libraries(Memory PUBLIC Common Container Threads::Threads)

function(core_memory_mode Variable Macro)
	set(${Variable} "AUTO" CACHE STRING "Memory diagnostic mode: AUTO, ON or OFF")
	set_property(CACHE ${Variable} PROPERTY STRINGS AUTO ON OFF)
	if("${${Variable}}" STREQUAL "AUTO")
		if(CMAKE_CONFIGURATION_TYPES)
			set(Mode "$<IF:$<CONFIG:Debug>,1,0>")
		elseif(CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
			set(Mode 1)
		else()
			set(Mode 0)
		endif()
	elseif("${${Variable}}" STREQUAL "ON")
		set(Mode 1)
	elseif("${${Variable}}" STREQUAL "OFF")
		set(Mode 0)
	else()
		message(FATAL_ERROR "${Variable} must be AUTO, ON or OFF")
	endif()
	target_compile_definitions(Memory PUBLIC "${Macro}=${Mode}")
endfunction()

core_memory_mode(CORE_MEMORY_DEBUG_GUARDS CORE_MEM_ENABLE_DEBUG_GUARDS)
core_memory_mode(CORE_MEMORY_UAF_DETECTION CORE_MEM_ENABLE_UAF_DETECTION)
core_memory_mode(CORE_MEMORY_CAPTURE_STACK CORE_MEM_CAPTURE_STACK)
option(CORE_MEMORY_ENABLE_GLOBAL_HOOKS "Enable process-wide allocation hooks" OFF)
target_compile_definitions(Memory PUBLIC
	CORE_MEM_ENABLE_GLOBAL_HOOKS=$<BOOL:${CORE_MEMORY_ENABLE_GLOBAL_HOOKS}>
	CORE_MEM_ENABLE_LAZY_COMMIT=1
	CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING=0
	CORE_MEM_QUARANTINE_RELEASE_ONLY_ON_FLUSH=0
)

add_library(Infra INTERFACE)
target_link_libraries(Infra
INTERFACE
	Math
	Memory
	Wrappers
	Container
	Reflect
	Serde
	Common
)
target_compile_features(Infra INTERFACE cxx_std_23)

add_library(Infra::Infra ALIAS Infra)
add_library(Infra::Math ALIAS Math)
add_library(Infra::Wrappers ALIAS Wrappers)
add_library(Infra::Container ALIAS Container)
add_library(Infra::Common ALIAS Common)
add_library(Infra::Memory ALIAS Memory)
add_library(Infra::Reflect ALIAS Reflect)
add_library(Infra::Serde ALIAS Serde)
