# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required(VERSION 3.21)

project ("kes")

function(bundle_runtime_libs target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    elseif(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "\$ORIGIN")
        # Use DT_RPATH (applies to transitive deps) instead of DT_RUNPATH
        target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
        get_property(_sdl3_dir GLOBAL PROPERTY KES_SDL3_LIBRARY_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DTARGET_FILE=$<TARGET_FILE:${target}>
                    -DOUT_DIR=$<TARGET_FILE_DIR:${target}>
                    -DEXTRA_DIRS=${_sdl3_dir}
                    -P "${CMAKE_SOURCE_DIR}/cmake/BundleRuntimeLibs.cmake")
    endif()
endfunction()

enable_testing()

# Project settings 
set(KES_PROJECT_NAME "kes" CACHE STRING "Game name: window title, package names")
set(KES_PROJECT_VERSION "1.0.0.0" CACHE STRING "Game version (four numbers, required by GDK packages)")
set(KES_PROJECT_PUBLISHER "KES" CACHE STRING "Publisher display name (GDK packages)")
set(KES_PAK_ENCRYPTION_KEY "" CACHE STRING "64 hex chars to encrypt packaged .pak files (empty = no encryption). Generate with: pak-tool <src> <out.pak> --generate-key")
if(KES_PAK_ENCRYPTION_KEY AND NOT KES_PAK_ENCRYPTION_KEY MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "KES: KES_PAK_ENCRYPTION_KEY must be hexadecimal.")
endif()
string(LENGTH "${KES_PAK_ENCRYPTION_KEY}" _kes_key_length)
if(KES_PAK_ENCRYPTION_KEY AND NOT _kes_key_length EQUAL 64)
    message(FATAL_ERROR "KES: KES_PAK_ENCRYPTION_KEY must be 64 hex characters (32 bytes).")
endif()
if(NOT KES_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "KES: KES_PROJECT_VERSION must look like 1.0.0.0")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(KesBuildChecks) # Compiler warnings and sanitizers
include(KesCrash)       # KES_CRASH_REPORTER: Crashpad minidumps, Release debug info
include(KesPlatform)    # KES_PLATFORM: Windows, GDK, GDKX, Linux
include(KesRenderers)   # KES_RENDERERS_<PLATFORM>: which renderer backends are built
include(KesShaders)     # Slang -> SPIR-V / DXBC at build time
include(KesPackage)     # "package" target: staged folder + archive / MSIXVC
include(KesProfiler)    # KES_PROFILING: Tracy profiler zones (engine/profiler.h)
include(KesUi)          # RmlUi + FreeType (HTML/CSS UI, engine/ui_system.h)
include(KesPhysics)     # Box3D (collision and rigid bodies, engine/collision_world.h)

# Debug hotkeys (F1-F4 render/input toggles, F7 UI reload, F8 RmlUi inspector)
# and the RmlUi debugger. On for development builds; the package-* presets turn
# it off so shipped games don't carry them (ENGINE_ALLOW_DEBUG_MODE).
option(KES_DEBUG_TOOLS "Build debug hotkeys and the UI inspector into the engine" ON)

# Include sub-projects.
add_subdirectory ("engine")
add_subdirectory ("game")

option(KES_BUILD_SAMPLES "Build the sample programs in samples/" ON)
if(KES_BUILD_SAMPLES)
    add_subdirectory ("samples")
endif()

option(KES_BUILD_TESTS "Build the unit tests in tests/" ON)
if(KES_BUILD_TESTS)
    add_subdirectory ("tests")
endif()
