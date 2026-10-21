# 游戏模块: 引擎托管并驱动的"游戏实体"。
#
# 依赖方向保持 game -> engine(通过 Runtime::Runtime 使用引擎头文件与日志),
# 引擎绝不反向依赖本模块 —— 引擎只通过 runtime::IGame 这一窄接口驱动游戏。
file(GLOB_RECURSE GameSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/*.hpp"
)

add_internal_library(Game
    TYPE                STATIC
    SOURCES             ${GameSources}
    INCLUDES            "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    PUBLIC_DEPENDENCY   Runtime::Runtime
    ALIAS               Game::Game
)
