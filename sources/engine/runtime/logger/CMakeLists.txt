file(GLOB_RECURSE SOURCES_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/includes/*.hpp
)

add_internal_library(Logger
    TYPE                STATIC
    SOURCES             ${SOURCES_LIST}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    PRIVATE_INCLUDES    "${CMAKE_CURRENT_SOURCE_DIR}/sources"
                        "${CMAKE_CURRENT_SOURCE_DIR}/includes/logger"
    PRIVATE_DEPENDENCY  Core::Core
    ALIAS               Runtime::Logger
)
# ALIAS 是内部的, INTERFACE 是外部的
# INTERFACE 目标可以被其他模块链接, 而 ALIAS 目标只能在当前 CMakeLists.txt 中使用
# Runtime_ 是 Runtime 下的统一约定
add_library(Runtime_Logger INTERFACE)
target_link_libraries(Runtime_Logger INTERFACE Logger)