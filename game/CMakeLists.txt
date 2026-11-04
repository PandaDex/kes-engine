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
kes_target_warnings(game)
kes_target_utf8(game)

if (CMAKE_VERSION VERSION_GREATER 3.21)
  set_property(TARGET game PROPERTY CXX_STANDARD 20)
  bundle_runtime_libs(game)
endif()


kes_add_game(game
    CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content"
    CONFIG_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/config"
    GDK_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/platform/gdk")


add_compile_definitions("K_STRUCT(...)=" "K_ENUM(...)=")
