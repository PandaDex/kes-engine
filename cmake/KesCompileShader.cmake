# Build-time driver for one .slang file (invoked by kes_add_slang_shaders).
#
#   cmake -DSLANGC=<slangc> -DSOURCE=<file.slang> -DOUTPUT_BASE=<dir/name>
#         -DFORMATS=spirv|dxbc -DINCLUDE_DIRS=<dir>|<dir> -DSTAMP=<file>
#         -P KesCompileShader.cmake
#
# Detects which stages the file defines, compiles each one for every format and
# removes binaries for stages that no longer exist. Include-only files (no
# entry points) produce nothing but the stamp.
cmake_minimum_required(VERSION 3.21)

foreach(_var SLANGC SOURCE OUTPUT_BASE STAMP)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "KesCompileShader: ${_var} not set")
    endif()
endforeach()

string(REPLACE "|" ";" FORMATS "${FORMATS}")
string(REPLACE "|" ";" INCLUDE_DIRS "${INCLUDE_DIRS}")

file(READ "${SOURCE}" _text)
set(_has_vs FALSE)
set(_has_ps FALSE)
if(_text MATCHES "\\[shader\\(\"vertex\"\\)\\]")
    set(_has_vs TRUE)
endif()
if(_text MATCHES "\\[shader\\(\"(fragment|pixel)\"\\)\\]")
    set(_has_ps TRUE)
endif()

set(_include_flags "")
foreach(_dir IN LISTS INCLUDE_DIRS)
    list(APPEND _include_flags "-I" "${_dir}")
endforeach()

get_filename_component(_out_dir "${OUTPUT_BASE}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

foreach(_stage vs ps)
    if(_stage STREQUAL "vs")
        set(_entry "vsMain")
        set(_slang_stage "vertex")
        set(_present ${_has_vs})
    else()
        set(_entry "psMain")
        set(_slang_stage "fragment")
        set(_present ${_has_ps})
    endif()

    foreach(_format spirv dxbc)
        # KES_TARGET_* lets shaders handle the rare per-backend difference.
        if(_format STREQUAL "spirv")
            set(_ext "spv")
            set(_target_flags -target spirv -profile spirv_1_3 -DKES_TARGET_SPIRV=1)
        else()
            set(_ext "dxbc")
            set(_target_flags -target dxbc -profile "${_stage}_5_0" -DKES_TARGET_DXBC=1)
        endif()
        set(_out "${OUTPUT_BASE}.${_stage}.${_ext}")

        if(NOT _present OR NOT _format IN_LIST FORMATS)
            file(REMOVE "${_out}")
            continue()
        endif()

        execute_process(
            COMMAND "${SLANGC}" "${SOURCE}"
                -entry "${_entry}" -stage "${_slang_stage}"
                ${_target_flags}
                -matrix-layout-row-major
                -O2
                ${_include_flags}
                -o "${_out}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr)

        if(_stdout)
            message("${_stdout}")
        endif()
        if(NOT _result EQUAL 0)
            file(REMOVE "${_out}")
            message(FATAL_ERROR "slangc failed for ${SOURCE} (${_entry}, ${_format}):\n${_stderr}")
        elseif(_stderr)
            message("${_stderr}")
        endif()
    endforeach()
endforeach()

if(NOT _has_vs AND NOT _has_ps)
    message(STATUS "Slang: ${SOURCE} has no entry points (include file)")
endif()

get_filename_component(_stamp_dir "${STAMP}" DIRECTORY)
file(MAKE_DIRECTORY "${_stamp_dir}")
file(TOUCH "${STAMP}")
