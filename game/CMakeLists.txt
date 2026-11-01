# CMakeLists.txt for the Game
#
set(GAME_SOURCES
    "src/main.cpp"
)

if(WIN32)
add_executable(game WIN32 ${GAME_SOURCES})
else()
add_executable(game ${GAME_SOURCES})
endif()

target_link_libraries(game PUBLIC engine)

if (CMAKE_VERSION VERSION_GREATER 3.21)
  set_property(TARGET game PROPERTY CXX_STANDARD 20)
  bundle_runtime_libs(game)
endif()


add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:game>/content"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/content"
        "$<TARGET_FILE_DIR:game>/content"
)

# Shaders: every *.slang under content/ is compiled at build time for all enabled
# renderers. Game shaders can #include the engine's kes_common.slang.
# Deployed after the content copy above (POST_BUILD steps run in order).
kes_add_slang_shaders(game_shaders
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/compiled_shaders/game"
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/shaders")

kes_deploy_shaders(game engine_shaders "content/engine/shaders")
kes_deploy_shaders(game game_shaders "content")


add_compile_definitions("K_STRUCT(...)=" "K_ENUM(...)=")
