# KES Slang shader pipeline.
#
# Shaders are written once in Slang and compiled at build time for every
# enabled renderer backend (KES_SHADER_FORMATS from KesRenderers.cmake):
#
#   foo.slang -> foo.vs.spv, foo.ps.spv     (Vulkan / SPIR-V)
#                foo.vs.dxbc, foo.ps.dxbc   (D3D11 / DXBC SM 5.0)
#
# Entry points: [shader("vertex")] vsMain and [shader("fragment")] psMain.
# Files without entry points (e.g. kes_common.slang) are treated as include
# files and only trigger recompiles of the shaders around them.
#
# Usage:
#   kes_add_slang_shaders(<name>
#       SOURCE_DIR <dir>          # scanned recursively for *.slang
#       OUTPUT_DIR <dir>          # binaries keep paths relative to SOURCE_DIR
#       [INCLUDE_DIRS <dir>...])  # extra #include search paths
#
#   kes_deploy_shaders(<executable> <name> <destination relative to the exe dir>)
#       copies the compiled binaries next to the executable after each build.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/KesRenderers.cmake")

if(WIN32)
    set(_kes_slang_root "${CMAKE_SOURCE_DIR}/external/slang/win64")
elseif(UNIX AND NOT APPLE)
    set(_kes_slang_root "${CMAKE_SOURCE_DIR}/external/slang/linux64")
else()
    set(_kes_slang_root "")
endif()

find_program(KES_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS "${_kes_slang_root}/bin"
    DOC "Slang shader compiler used to build KES shaders")

if(NOT KES_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "KES: slangc not found in ${_kes_slang_root}/bin.\n"
        "Download the external binaries first:\n"
        "  Windows: powershell -ExecutionPolicy Bypass -File external/download_external.ps1\n"
        "  Linux:   ./external/download_external.sh\n"
        "or set KES_SLANGC_EXECUTABLE to an existing slangc.")
endif()
message(STATUS "KES slangc: ${KES_SLANGC_EXECUTABLE}")

set(_KES_COMPILE_SHADER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/KesCompileShader.cmake")

function(kes_add_slang_shaders name)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;OUTPUT_DIR" "INCLUDE_DIRS" ${ARGN})
    if(NOT ARG_SOURCE_DIR OR NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "kes_add_slang_shaders(${name}): SOURCE_DIR and OUTPUT_DIR are required")
    endif()

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${ARG_SOURCE_DIR}/*.slang")

    # Any .slang reachable through the include path can be #included, so treat
    # all of them as dependencies. Coarse but reliable and cheap for PSX-scale
    # shader counts.
    set(_include_args "${ARG_SOURCE_DIR}")
    set(_all_slang ${_sources})
    foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
        file(GLOB_RECURSE _inc CONFIGURE_DEPENDS "${_dir}/*.slang")
        list(APPEND _all_slang ${_inc})
        list(APPEND _include_args "${_dir}")
    endforeach()
    list(REMOVE_DUPLICATES _all_slang)
    string(REPLACE ";" "|" _include_args "${_include_args}")
    string(REPLACE ";" "|" _formats "${KES_SHADER_FORMATS}")

    set(_stamps "")
    foreach(_src IN LISTS _sources)
        file(RELATIVE_PATH _rel "${ARG_SOURCE_DIR}" "${_src}")
        string(REGEX REPLACE "\\.slang$" "" _rel_base "${_rel}")
        set(_out_base "${ARG_OUTPUT_DIR}/${_rel_base}")
        set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${name}.stamps/${_rel_base}.stamp")

        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND "${CMAKE_COMMAND}"
                "-DSLANGC=${KES_SLANGC_EXECUTABLE}"
                "-DSOURCE=${_src}"
                "-DOUTPUT_BASE=${_out_base}"
                "-DFORMATS=${_formats}"
                "-DINCLUDE_DIRS=${_include_args}"
                "-DSTAMP=${_stamp}"
                -P "${_KES_COMPILE_SHADER_SCRIPT}"
            DEPENDS ${_all_slang} "${_KES_COMPILE_SHADER_SCRIPT}"
            COMMENT "Slang: ${_rel}"
            VERBATIM)
        list(APPEND _stamps "${_stamp}")
    endforeach()

    add_custom_target(${name} ALL DEPENDS ${_stamps} SOURCES ${_sources})
    set_target_properties(${name} PROPERTIES
        KES_SHADER_OUTPUT_DIR "${ARG_OUTPUT_DIR}"
        FOLDER "Shaders")
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
endfunction()

function(kes_deploy_shaders executable shader_target destination)
    add_dependencies(${executable} ${shader_target})
    # Generator expression so the shader target may be defined in a directory
    # processed after this one.
    add_custom_command(TARGET ${executable} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${executable}>/${destination}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_PROPERTY:${shader_target},KES_SHADER_OUTPUT_DIR>"
            "$<TARGET_FILE_DIR:${executable}>/${destination}"
        COMMENT "Deploying ${shader_target} to ${destination}"
        VERBATIM)
endfunction()
