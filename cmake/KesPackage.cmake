# KES game targets and packaging.
#
#   kes_add_game(<target>
#       CONTENT_DIR <dir>        # game content (maps, meshes, *.slang, ...)
#       [CONFIG_DIR <dir>]       # shipped next to the exe as config/ (input maps, ...)
#       [GDK_DIR <dir>]          # MicrosoftGame.config.in + logos (KES_PLATFORM=GDK)
#       [EXTRA_CONTENT_DIRS <dir>...]  # copied under CONTENT_DIR first (dev layout only,
#                                      # no shader compile), e.g. to share another game's maps
#       [NO_PACKAGE])            # no kes_package target (samples, extra game targets)
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
#        <exe>, runtime libraries, crash handler, config/, content/game.pak, content/engine.pak
#   -> plus the platform format (KES_PACKAGE_FORMAT_<PLATFORM>):
#        zip     <name>-<platform>-<version>.zip
#        tar.gz  <name>-<platform>-<version>.tar.gz
#        msixvc  msixvc/*.msixvc via makepkg (GDK)
#        folder  staged folder only
#   -> <name>-<platform>-symbols-<version>.zip   PDBs / .debug files for crash reports.
#      Keep one per release and don't ship it.
#   Paks are encrypted when KES_PAK_ENCRYPTION_KEY is set.
#   Any configuration packages (Debug too). The KES_PACKAGE_* options below change
#   what goes in; tools/buildLauncher sets them per profile.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/KesPlatform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/KesShaders.cmake")

set(KES_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/out/package" CACHE PATH "Where kes_package writes packages")
set(KES_PACKAGE_FORMAT_WINDOWS "zip"    CACHE STRING "Package format for Windows (zip, folder)")
set(KES_PACKAGE_FORMAT_GDK     "msixvc" CACHE STRING "Package format for GDK (msixvc, zip, folder)")
set(KES_PACKAGE_FORMAT_LINUX   "tar.gz" CACHE STRING "Package format for Linux (tar.gz, zip, folder)")
set(KES_PACKAGE_FORMAT_MACOS   "zip"    CACHE STRING "Package format for macOS (zip, folder)")

set(KES_PACKAGE_CONTENT "pak" CACHE STRING "How packages carry content: pak (content/*.pak) or loose (plain files under content/)")
set_property(CACHE KES_PACKAGE_CONTENT PROPERTY STRINGS pak loose)
set(KES_PACKAGE_SYMBOLS "archive" CACHE STRING "Debug symbols: archive (separate zip, not shipped), package (next to the binaries), both, none")
set_property(CACHE KES_PACKAGE_SYMBOLS PROPERTY STRINGS archive package both none)
set(KES_PACKAGE_SUFFIX "" CACHE STRING "Appended to package names: <name>-<platform><suffix>")
option(KES_PACKAGE_STRIP_SOURCES "Leave shader sources (.slang, .slangnodes) and map sources (.fmap) out of packages" ON)
option(KES_PACKAGE_VC_RUNTIME "Copy the MSVC runtime DLLs into Windows packages (the debug runtime for Debug builds, which Microsoft doesn't allow redistributing)" OFF)
if(NOT KES_PACKAGE_CONTENT MATCHES "^(pak|loose)$")
    message(FATAL_ERROR "KES_PACKAGE_CONTENT must be pak or loose, not '${KES_PACKAGE_CONTENT}'")
endif()
if(NOT KES_PACKAGE_SYMBOLS MATCHES "^(archive|package|both|none)$")
    message(FATAL_ERROR "KES_PACKAGE_SYMBOLS must be archive, package, both or none, not '${KES_PACKAGE_SYMBOLS}'")
endif()

# Release and debug MSVC runtime DLL lists, written to a file the staging script reads
# (paths with spaces don't survive the custom command line). Picked per configuration.
function(_kes_write_msvc_runtime_list file)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
    include(InstallRequiredSystemLibraries)
    set(_release ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
    set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
    include(InstallRequiredSystemLibraries)
    file(WRITE "${file}"
        "set(VC_RUNTIME_RELEASE [==[${_release}]==])
set(VC_RUNTIME_DEBUG [==[${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}]==])
")
endfunction()

# GDK identity (MicrosoftGame.config). Placeholders work for local builds.
set(KES_GDK_IDENTITY_NAME "KES.Game"      CACHE STRING "GDK package identity name (3-50 chars: letters, digits, '.', '-')")
set(KES_GDK_PUBLISHER     "CN=Publisher"  CACHE STRING "GDK publisher (distinguished name from Partner Center)")
set(KES_GDK_MSA_APP_ID    "PleaseChangeMe" CACHE STRING "GDK MSAAppId from Partner Center")
set(KES_GDK_TITLE_ID      "FFFFFFFF"      CACHE STRING "GDK TitleId from Partner Center")

set(_KES_PACKAGE_STAGE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/KesPackageStage.cmake")

function(kes_add_game target)
    cmake_parse_arguments(ARG "NO_PACKAGE" "CONTENT_DIR;CONFIG_DIR;GDK_DIR" "EXTRA_CONTENT_DIRS" ${ARGN})
    if(NOT ARG_CONTENT_DIR)
        message(FATAL_ERROR "kes_add_game(${target}): CONTENT_DIR is required")
    endif()

    set(_engine_assets "${CMAKE_SOURCE_DIR}/engine/assets")
    set(_exe_dir "$<TARGET_FILE_DIR:${target}>")

    #  Shaders: everything under CONTENT_DIR/**.slang
    kes_add_slang_shaders(${target}_shaders
        SOURCE_DIR "${ARG_CONTENT_DIR}"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/compiled_shaders/${target}"
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/shaders")

    # Development layout next to the exe. A custom target without outputs runs on
    # every build, so content edits show up even when nothing recompiles.
    set(_extra_content_commands "")
    foreach(_extra_dir IN LISTS ARG_EXTRA_CONTENT_DIRS)
        list(APPEND _extra_content_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_extra_dir}" "${_exe_dir}/content")
    endforeach()
    set(_config_commands "")
    if(ARG_CONFIG_DIR)
        set(_config_commands
            COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_exe_dir}/config"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ARG_CONFIG_DIR}" "${_exe_dir}/config")
    endif()
    add_custom_target(${target}_content ALL
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_exe_dir}/content"
        ${_extra_content_commands}
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ARG_CONTENT_DIR}" "${_exe_dir}/content"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_engine_assets}" "${_exe_dir}/content"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_PROPERTY:engine_shaders,KES_SHADER_OUTPUT_DIR>" "${_exe_dir}/content/engine/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_PROPERTY:${target}_shaders,KES_SHADER_OUTPUT_DIR>" "${_exe_dir}/content"
        ${_config_commands}
        COMMENT "Copying content and config for ${target}"
        VERBATIM)
    add_dependencies(${target}_content engine_shaders ${target}_shaders)
    add_dependencies(${target} ${target}_content)

    kes_deploy_crash_handler(${target})

    # Platform files copied to the package root 
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

    if(ARG_NO_PACKAGE)
        return()
    endif()

    # kes_package 
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
    string(REPLACE ";" "|" _crash_files_arg "${KES_CRASH_RUNTIME_FILES}")
    set(_symbol_files_arg "")
    if(MSVC)
        set(_symbol_files_arg "$<TARGET_PDB_FILE:${target}>")
        if(KES_ENGINE_SHARED)
            string(APPEND _symbol_files_arg "|$<TARGET_PDB_FILE:engine>")
        endif()
    endif()
    set(_vc_runtime_list "")
    if(MSVC AND KES_PACKAGE_VC_RUNTIME)
        set(_vc_runtime_list "${CMAKE_CURRENT_BINARY_DIR}/kes_msvc_runtime.cmake")
        _kes_write_msvc_runtime_list("${_vc_runtime_list}")
    endif()
    set(_split_libs_arg "")
    if(KES_ENGINE_SHARED AND NOT WIN32)
        set(_split_libs_arg "$<TARGET_FILE_NAME:engine>")
    endif()

    add_custom_target(kes_package
        COMMAND "${CMAKE_COMMAND}"
            "-DPLATFORM=${_platform_lower}"
            "-DCONFIG=$<CONFIG>"
            "-DNAME=${KES_PROJECT_NAME}"
            "-DVERSION=${KES_PROJECT_VERSION}"
            "-DEXE=$<TARGET_FILE:${target}>"
            "-DEXE_DIR=${_exe_dir}"
            "-DRUNTIME_FILES=$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,|>"
            "-DCRASH_FILES=${_crash_files_arg}"
            "-DSYMBOL_FILES=${_symbol_files_arg}"
            "-DSPLIT_LIBS=${_split_libs_arg}"
            "-DOBJCOPY=${CMAKE_OBJCOPY}"
            "-DPAK_TOOL=$<TARGET_FILE:pak-tool>"
            "-DPAK_KEY=${KES_PAK_ENCRYPTION_KEY}"
            "-DGAME_CONTENT_DIR=${ARG_CONTENT_DIR}"
            "-DGAME_SHADERS_DIR=${CMAKE_BINARY_DIR}/compiled_shaders/${target}"
            "-DENGINE_ASSETS_DIR=${_engine_assets}"
            "-DENGINE_SHADERS_DIR=$<TARGET_PROPERTY:engine_shaders,KES_SHADER_OUTPUT_DIR>"
            "-DCONFIG_DIR=${ARG_CONFIG_DIR}"
            "-DPLATFORM_FILES=${_platform_files_arg}"
            "-DFORMAT=${_format}"
            "-DCONTENT_MODE=${KES_PACKAGE_CONTENT}"
            "-DSYMBOLS_MODE=${KES_PACKAGE_SYMBOLS}"
            "-DSUFFIX=${KES_PACKAGE_SUFFIX}"
            "-DSTRIP_SOURCES=${KES_PACKAGE_STRIP_SOURCES}"
            "-DVC_RUNTIME_LIST=${_vc_runtime_list}"
            "-DOUTPUT_DIR=${KES_PACKAGE_DIR}"
            "-DMAKEPKG=${KES_GDK_MAKEPKG}"
            -P "${_KES_PACKAGE_STAGE_SCRIPT}"
        DEPENDS "${_KES_PACKAGE_STAGE_SCRIPT}"
        COMMENT "Packaging ${KES_PROJECT_NAME} for ${KES_PLATFORM} ($<CONFIG>, ${_format}, ${KES_PACKAGE_CONTENT} content)"
        VERBATIM)
    add_dependencies(kes_package ${target} pak-tool ${target}_shaders engine_shaders ${KES_CRASH_RUNTIME_TARGETS})
endfunction()
