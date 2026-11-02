# UI showcase: a small "game" built around the RmlUi based UI system.
# See samples/ui/README.md.
set(UI_SHOWCASE_SOURCES
    "src/main.cpp"
)

if(WIN32)
    add_executable(ui_showcase WIN32 ${UI_SHOWCASE_SOURCES})
else()
    add_executable(ui_showcase ${UI_SHOWCASE_SOURCES})
endif()

target_link_libraries(ui_showcase PUBLIC engine)
set_property(TARGET ui_showcase PROPERTY CXX_STANDARD 20)
if(WIN32)
    target_compile_definitions(ui_showcase PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
bundle_runtime_libs(ui_showcase)

# The game's content (startup map, meshes, materials) is copied first so the
# menus and HUD are drawn over a live 3D scene; the sample's own content/ui
# goes on top. Input map and default settings come from the game as well.
kes_add_game(ui_showcase
    CONTENT_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/content"
    CONFIG_DIR         "${CMAKE_SOURCE_DIR}/game/config"
    GDK_DIR            "${CMAKE_CURRENT_SOURCE_DIR}/platform/gdk"
    NO_PACKAGE)
