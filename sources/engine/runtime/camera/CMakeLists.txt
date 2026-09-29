# Camera 目前是纯头文件模块(Camera.hpp), 暂无可编译的 .cpp.
# 因此用 INTERFACE 目标: 只对外提供头文件搜索路径, 不产生静态库产物.
# 后续补充 Camera.cpp / CameraManager.cpp 时, 改为:
#     file(GLOB_RECURSE SOURCES_LIST ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp)
#     add_internal_library(Camera TYPE STATIC SOURCES ${SOURCES_LIST} ...)
add_internal_library(Camera
    TYPE                INTERFACE
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC_DEPENDENCY   Core::Core
    ALIAS               Runtime::Camera
)
# ALIAS 是内部的, INTERFACE 是外部的
# INTERFACE 目标可以被其他模块链接, 而 ALIAS 目标只能在当前 CMakeLists.txt 中使用
# Runtime_ 是 Runtime 下的统一约定
add_library(Runtime_Camera INTERFACE)
target_link_libraries(Runtime_Camera INTERFACE Camera)
