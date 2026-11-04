# CMakeList.txt : CMake project for engine, include source and define
# project specific logic here.
project(engine)
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/SDL3")
endif()
find_package(SDL3 REQUIRED CONFIG)

# Imported targets are scoped to this directory, so bundle_runtime_libs() can't
# ask for SDL's location from game/ or tests/.
if(UNIX AND NOT APPLE)
    get_target_property(_kes_sdl3_location SDL3::SDL3 LOCATION)
    get_filename_component(_kes_sdl3_dir "${_kes_sdl3_location}" DIRECTORY)
    set_property(GLOBAL PROPERTY KES_SDL3_LIBRARY_DIR "${_kes_sdl3_dir}")
endif()

# Renderer backends come from KES_RENDERERS_<PLATFORM> (cmake/KesRenderers.cmake).
if(KES_RENDERER_VULKAN)
    find_package(Vulkan REQUIRED)
endif()

add_library(engine STATIC
    "engine.h"
    "engine.cpp"
    "engine_configuration.h"
    "shared.h"
    "window.h"
    "window.cpp"
    "input.h"
    "input.cpp"
    "render_backend.h"
    "render_backend.cpp"
    "render_common.cpp"
    "build_info.h"
    "build_info.cpp"
    "crash_reporter.h"
    "crash_reporter.cpp"
    "command_line.h"
    "command_line.cpp"
    "ini_file.h"
    "ini_file.cpp"
    "game_settings.h"
    "game_settings.cpp"
    "shader_binary.h"
    "shader_binary.cpp"
    "camera.h"
    "camera.cpp"
    "ent_camera.h"
    "ent_camera.cpp"
    "animation_clip.h"
    "animation_clip.cpp"
    "animation_graph.h"
    "animation_graph.cpp"
    "animation_pose.h"
    "animation_pose.cpp"
    "animator.h"
    "animator.cpp"
    "ent_audio_source.h"
    "ent_audio_source.cpp"
    "ent_player.h"
    "ent_player.cpp"
    "ent_player_start.h"
    "ent_player_start.cpp"
    "ent_skinned_model.h"
    "ent_skinned_model.cpp"
    "ent_trigger.h"
    "ent_trigger.cpp"
    "ent_brush.h"
    "ent_brush.cpp"
    "ent_light.h"
    "ent_light.cpp"
    "ent_skybox.h"
    "ent_skybox.cpp"
    "ent_fog_volume.h"
    "ent_fog_volume.cpp"
    "entity_property_utils.h"
    "text_parse_utils.h"
    "entity.h"
    "entity.cpp"
    "entity_manager.h"
    "entity_manager.cpp"
    "entity_factory_registry.h"
    "entity_factory_registry.cpp"
    "world.h"
    "world.cpp"
    "scene.h"
    "scene.cpp"
    "mesh.h"
    "mesh_file.h"
    "mesh_file.cpp"
    "kmodel.h"
    "kmodel.cpp"
    "material.h"
    "material_file.h"
    "material_file.cpp"
    "texture_file.h"
    "texture_file.cpp"
    "sound_file.h"
    "sound_file.cpp"
    "asset_system.h"
    "asset_system.cpp"
    "shader.h"
    "shader.cpp"
    "text_block_file.h"
    "text_block_file.cpp"
    "engine_types.h"
    "game_context.h"
    "game_context.cpp"
    "logger.h"
    "logger.cpp"
    "file_paths.h"
    "file_paths.cpp"
    "pak_file_system.h"
    "pak_file_system.cpp"
    "binary_reader.h"
    "map_file.h"
    "map_file.cpp"
    "project_file.h"
    "project_file.cpp"
    "collision_world.h"
    "collision_world.cpp"
    "audio_system.h"
    "audio_system.cpp"
    "ui_system.h"
    "ui_system.cpp"
    "platform/platform.h"
    "profiler.h"
        reflection.h
)
target_include_directories(engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(engine PUBLIC SDL3::SDL3)

# UI (cmake/KesUi.cmake). RmlUi is PUBLIC: game code uses the Rml API on the
# documents and elements UiSystem hands out.
target_sources(engine PRIVATE
        "${KES_RMLUI_BACKENDS_DIR}/RmlUi_Platform_SDL.h"
        "${KES_RMLUI_BACKENDS_DIR}/RmlUi_Platform_SDL.cpp"
)
target_include_directories(engine PRIVATE "${KES_RMLUI_BACKENDS_DIR}")
target_compile_definitions(engine PRIVATE RMLUI_SDL_VERSION_MAJOR=3)
target_link_libraries(engine PUBLIC RmlUi::RmlUi)

# Physics (cmake/KesPhysics.cmake)
target_link_libraries(engine PRIVATE box3d::box3d)

# Crash reporting (KES_CRASH_REPORTER, cmake/KesCrash.cmake)
kes_target_crash_reporter(engine)
if(TARGET sentry::sentry)
    set(_kes_crash_reporter 1)
else()
    set(_kes_crash_reporter 0)
endif()
set_source_files_properties("crash_reporter.cpp" PROPERTIES COMPILE_DEFINITIONS
    "KES_CRASH_REPORTER=${_kes_crash_reporter};KES_CRASH_DSN=\"${KES_CRASH_DSN}\"")

# Build identity (version, commit), refreshed on every build.
find_package(Git QUIET)
set(_kes_build_info_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(kes_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DOUT=${_kes_build_info_dir}/kes_build_info.h"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DGIT=${GIT_EXECUTABLE}"
        "-DNAME=${KES_PROJECT_NAME}"
        "-DVERSION=${KES_PROJECT_VERSION}"
        "-DCONFIG=$<CONFIG>"
        "-DPLATFORM=${KES_PLATFORM}"
        -P "${CMAKE_SOURCE_DIR}/cmake/KesBuildInfo.cmake"
    BYPRODUCTS "${_kes_build_info_dir}/kes_build_info.h"
    VERBATIM)
add_dependencies(engine kes_build_info)
target_include_directories(engine PRIVATE "${_kes_build_info_dir}")

# Profiler (KES_PROFILING, cmake/KesProfiler.cmake)
kes_target_profiler(engine)

# Renderer backends
if(KES_RENDERER_VULKAN)
    target_sources(engine PRIVATE "vulkan_render.h" "vulkan_render.cpp")
    target_link_libraries(engine PRIVATE Vulkan::Vulkan)
endif()

if(KES_RENDERER_D3D11)
    target_sources(engine PRIVATE "d3d11_render.h" "d3d11_render.cpp")
    target_link_libraries(engine PRIVATE d3d11 dxgi)
endif()

if(KES_ALLOW_RENDERER_SELECTION)
    set(_kes_allow_selection 1)
else()
    set(_kes_allow_selection 0)
endif()
string(REPLACE ";" "," _kes_renderer_priority "${KES_RENDERERS}")
set_source_files_properties("render_backend.cpp" PROPERTIES COMPILE_DEFINITIONS
    "KES_HAS_RENDERER_VULKAN=$<BOOL:${KES_RENDERER_VULKAN}>;KES_HAS_RENDERER_D3D11=$<BOOL:${KES_RENDERER_D3D11}>;KES_RENDERER_PRIORITY=\"${_kes_renderer_priority}\";KES_RENDERER_ALLOW_SELECTION=${_kes_allow_selection}")

# Built-in shaders (Slang, compiled at build time)
# Deployed by games to <exe>/content/engine/shaders via kes_deploy_shaders().
kes_add_slang_shaders(engine_shaders
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/compiled_shaders/engine/shaders")
add_dependencies(engine engine_shaders)

set_property(TARGET engine PROPERTY CXX_STANDARD 20)
kes_target_warnings(engine)

# Platform (KES_PLATFORM, cmake/KesPlatform.cmake)
# Definitions are PUBLIC: headers such as file_paths.h branch on them, so tools
# and games linking the engine must see the same values.
target_compile_definitions(engine PUBLIC ${KES_PLATFORM_DEFINITIONS})

if(KES_PLATFORM STREQUAL "Windows" OR KES_PLATFORM STREQUAL "GDK")
    target_sources(engine PRIVATE
            "platform/platform_win.cpp"
            "platform/platform_win.h"
    )
endif()

if(KES_PLATFORM STREQUAL "GDK")
    # Gaming Runtime (XGameRuntimeInitialize etc.). xgameruntime.lib loads the
    # runtime dynamically and reports E_GAMERUNTIME_NOT_INSTALLED if missing.
    target_sources(engine PRIVATE
            "platform/platform_gdk.cpp"
            "platform/platform_gdk.h"
    )
    target_link_libraries(engine PUBLIC Kes::GDK)
endif()

if(KES_PLATFORM STREQUAL "Linux")
    target_sources(engine PRIVATE
            "platform/platform_linux.cpp"
            "platform/platform_linux.h"
    )
endif()

# Project settings baked into the engine (engine_configuration.h) 
# PUBLIC: engine and game code must agree (engine_configuration.h).
if(KES_DEBUG_TOOLS)
    target_compile_definitions(engine PUBLIC ENGINE_ALLOW_DEBUG_MODE=1)
else()
    target_compile_definitions(engine PUBLIC ENGINE_ALLOW_DEBUG_MODE=0)
endif()

target_compile_definitions(engine PRIVATE
        "ENGINE_PROJECT_NAME=\"${KES_PROJECT_NAME}\""
        "ENGINE_COMPANY_NAME=\"${KES_PROJECT_PUBLISHER}\""
        "ENGINE_PAK_ENCRYPTION_KEY_HEX=\"${KES_PAK_ENCRYPTION_KEY}\""
)

# Tools 
add_subdirectory ("tools/pak-tool")
if(WIN32)
add_subdirectory ("tools/model-importer")
endif()
