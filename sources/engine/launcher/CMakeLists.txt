add_subdirectory(generic)
add_subdirectory(editor)

set(TARGET_NAME Launcher)

add_executable(${TARGET_NAME})

add_custom_command(TARGET Launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:Launcher>
            $<TARGET_RUNTIME_DLLS:Launcher>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy runtime DLLs next to Launcher"
)

if(WIN32)
    add_custom_command(TARGET Launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/lib $<TARGET_FILE_DIR:Launcher>
    )
endif()

SeedCopyResources(${TARGET_NAME})

target_sources(${TARGET_NAME} PRIVATE Main.cpp)

target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${TARGET_NAME} PRIVATE
    Editor::Editor
    Generic::Generic
    Infra::Infra
    Runtime::Runtime
    Game::Game
    SDL3::SDL3
)