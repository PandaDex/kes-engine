# KES game targets and packaging.
#
#   kes_add_game(<target>
#       CONTENT_DIR <dir>        # game content (maps, meshes, *.slang, ...)
#       [CONFIG_DIR <dir>]       # shipped next to the exe as config/ (input maps, ...)
#       [GDK_DIR <dir>])         # MicrosoftGame.config.in + logos (KES_PLATFORM=GDK)
#
# Development builds (every normal build of <target>):
#   <exe dir>/content/              loose game content + compiled game shaders
#   <exe dir>/content/engine/       loose engine assets + compiled engine shaders
#   <exe dir>/config/               CONFIG_DIR
#   <exe dir>/MicrosoftGame.config  + logos (GDK only; the Gaming Runtime needs them)
#
# Packaging ("kes_package" target, built on demand):
#   cmake --build <build> --config Release --target kes_package
#   -> ${KES_PACKAGE_DIR}/<name>-<platform>/   staged, shippable folder
#        <exe>, runtime libraries, config/, content/game.pak, content/engine.pak
#   -> plus the platform format (KES_PACKAGE_FORMAT_<PLATFORM>):
#        zip     <name>-<platform>-<version>.zip
#        tar.gz  <name>-<platform>-<version>.tar.gz
#        msixvc  msixvc/*.msixvc via makepkg (GDK)
#        folder  staged folder only
#   Paks are encrypted when KES_PAK_ENCRYPTION_KEY is set.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/KesPlatform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/KesShaders.cmake")

set(KES_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/out/package" CACHE PATH "Where kes_package writes packages")
set(KES_PACKAGE_FORMAT_WINDOWS "zip"    CACHE STRING "Package format for Windows (zip, folder)")
set(KES_PACKAGE_FORMAT_GDK     "msixvc" CACHE STRING "Package format for GDK (msixvc, zip, folder)")
set(KES_PACKAGE_FORMAT_LINUX   "tar.gz" CACHE STRING "Package format for Linux (tar.gz, zip, folder)")
set(KES_PACKAGE_FORMAT_MACOS   "zip"    CACHE STRING "Package format for macOS (zip, folder)")

# GDK identity (MicrosoftGame.config). Placeholders work for local builds.
set(KES_GDK_IDENTITY_NAME "KES.Game"      CACHE STRING "GDK package identity name (3-50 chars: letters, digits, '.', '-')")
set(KES_GDK_PUBLISHER     "CN=Publisher"  CACHE STRING "GDK publisher (distinguished name from Partner Center)")
set(KES_GDK_MSA_APP_ID    "PleaseChangeMe" CACHE STRING "GDK MSAAppId from Partner Center")
set(KES_GDK_TITLE_ID      "FFFFFFFF"      CACHE STRING "GDK TitleId from Partner Center")

set(_KES_PACKAGE_STAGE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/KesPackageStage.cmake")

function(kes_add_game target)
    cmake_parse_arguments(ARG "" "CONTENT_DIR;CONFIG_DIR;GDK_DIR" "" ${ARGN})
    if(NOT ARG_CONTENT_DIR)
        message(FATAL_ERROR "kes_add_game(${target}): CONTENT_DIR is required")
    endif()

    set(_engine_assets "${CMAKE_SOURCE_DIR}/engine/assets")
    set(_exe_dir "$<TARGET_FILE_DIR:${target}>")

    # --- Shaders: everything under CONTENT_DIR/**.slang ---------------------
    kes_add_slang_shaders(${target}_shaders
        SOURCE_DIR "${ARG_CONTENT_DIR}"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/compiled_shaders/${target}"
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/shaders")

    # --- Development layout next to the exe (POST_BUILD steps run in order) --
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_exe_dir}/content"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ARG_CONTENT_DIR}" "${_exe_dir}/content"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_engine_assets}" "${_exe_dir}/content"
        COMMENT "Copying content for ${target}"
        VERBATIM)
    kes_deploy_shaders(${target} engine_shaders "content/engine/shaders")
    kes_deploy_shaders(${target} ${target}_shaders "content")

    if(ARG_CONFIG_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_exe_dir}/config"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ARG_CONFIG_DIR}" "${_exe_dir}/config"
            VERBATIM)
    endif()

    # --- Platform files copied to the package root -------------------------
    set(_platform_files "")
    if(KES_PLATFORM STREQUAL "GDK")
        if(NOT ARG_GDK_DIR OR NOT EXISTS "${ARG_GDK_DIR}/MicrosoftGame.config.in")
            message(FATAL_ERROR "kes_add_game(${target}): KES_PLATFORM=GDK needs GDK_DIR with MicrosoftGame.config.in")
        endif()
        if(NOT KES_GDK_IDENTITY_NAME MATCHES "^[A-Za-z0-9.-]+$")
            message(FATAL_ERROR "KES_GDK_IDENTITY_NAME may only contain letters, digits, '.' and '-'.")
        endif()

        get_target_property(_output_name ${target} OUTPUT_NAME)
        if(NOT _output_name)
            set(_output_name "${target}")
        endif()
        set(KES_GDK_EXECUTABLE "${_output_name}.exe")
        set(_gdk_out "${CMAKE_CURRENT_BINARY_DIR}/gdk")
        configure_file("${ARG_GDK_DIR}/MicrosoftGame.config.in" "${_gdk_out}/MicrosoftGame.config" @ONLY)

        file(GLOB _gdk_images "${ARG_GDK_DIR}/*.png")
        list(APPEND _platform_files "${_gdk_out}/MicrosoftGame.config" ${_gdk_images})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_platform_files} "${_exe_dir}"
            COMMENT "Copying MicrosoftGame.config and logos"
            VERBATIM)
    endif()

    # --- kes_package --------------------------------------------------------
    set(_format "${KES_PACKAGE_FORMAT_${KES_PLATFORM_ID}}")
    if(_format STREQUAL "msixvc" AND NOT KES_PLATFORM STREQUAL "GDK")
        message(FATAL_ERROR "KES: msixvc packages are only supported for KES_PLATFORM=GDK")
    endif()
    if(_format STREQUAL "msixvc" AND NOT KES_GDK_MAKEPKG)
        message(WARNING "KES: makepkg not found; kes_package will produce a folder instead of msixvc")
        set(_format "folder")
    endif()

    string(REPLACE ";" "|" _platform_files_arg "${_platform_files}")
    string(TOLOWER "${KES_PLATFORM}" _platform_lower)

    add_custom_target(kes_package
        COMMAND "${CMAKE_COMMAND}"
            "-DPLATFORM=${_platform_lower}"
            "-DCONFIG=$<CONFIG>"
            "-DNAME=${KES_PROJECT_NAME}"
            "-DVERSION=${KES_PROJECT_VERSION}"
            "-DEXE=$<TARGET_FILE:${target}>"
            "-DEXE_DIR=${_exe_dir}"
            "-DRUNTIME_FILES=$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,|>"
            "-DPAK_TOOL=$<TARGET_FILE:pak-tool>"
            "-DPAK_KEY=${KES_PAK_ENCRYPTION_KEY}"
            "-DGAME_CONTENT_DIR=${ARG_CONTENT_DIR}"
            "-DGAME_SHADERS_DIR=${CMAKE_BINARY_DIR}/compiled_shaders/${target}"
            "-DENGINE_ASSETS_DIR=${_engine_assets}"
            "-DENGINE_SHADERS_DIR=$<TARGET_PROPERTY:engine_shaders,KES_SHADER_OUTPUT_DIR>"
            "-DCONFIG_DIR=${ARG_CONFIG_DIR}"
            "-DPLATFORM_FILES=${_platform_files_arg}"
            "-DFORMAT=${_format}"
            "-DOUTPUT_DIR=${KES_PACKAGE_DIR}"
            "-DMAKEPKG=${KES_GDK_MAKEPKG}"
            -P "${_KES_PACKAGE_STAGE_SCRIPT}"
        DEPENDS "${_KES_PACKAGE_STAGE_SCRIPT}"
        COMMENT "Packaging ${KES_PROJECT_NAME} for ${KES_PLATFORM} ($<CONFIG>, ${_format})"
        VERBATIM)
    add_dependencies(kes_package ${target} pak-tool ${target}_shaders engine_shaders)
endfunction()
