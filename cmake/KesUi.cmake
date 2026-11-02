# KES UI: RmlUi (HTML/CSS-like documents, external/RmlUi) with the FreeType
# font engine (external/freetype). Both are git submodules built from source
# as static libraries, so the UI works on every platform the engine builds for.
#
#   git submodule update --init external/RmlUi external/freetype
#
# Provides the target RmlUi::RmlUi (core + debugger) and
# KES_RMLUI_BACKENDS_DIR (RmlUi's SDL platform glue, compiled into the engine).

include_guard(GLOBAL)

set(_kes_rmlui_dir "${CMAKE_SOURCE_DIR}/external/RmlUi")
set(_kes_freetype_dir "${CMAKE_SOURCE_DIR}/external/freetype")
foreach(_kes_dep_dir IN ITEMS "${_kes_rmlui_dir}" "${_kes_freetype_dir}")
    if(NOT EXISTS "${_kes_dep_dir}/CMakeLists.txt")
        message(FATAL_ERROR "KES: UI dependency missing: ${_kes_dep_dir}\n"
            "Fetch the submodules: git submodule update --init external/RmlUi external/freetype")
    endif()
endforeach()

# Both projects read these as option() defaults; normal variables win (CMP0077).
set(_kes_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

# FreeType: TrueType/OpenType rendering only, no optional system libraries,
# so the build is identical everywhere.
set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BROTLI ON)
set(SKIP_INSTALL_ALL ON)
add_subdirectory("${_kes_freetype_dir}" "${CMAKE_BINARY_DIR}/external/freetype" EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
# RmlUi calls find_package(Freetype); use the target above instead of a
# system copy.
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)

set(RMLUI_FONT_ENGINE "freetype")
set(RMLUI_SAMPLES OFF)
set(RMLUI_LUA_BINDINGS OFF)
set(RMLUI_SVG_PLUGIN OFF)
set(RMLUI_LOTTIE_PLUGIN OFF)
set(RMLUI_TRACY_PROFILING OFF)
set(RMLUI_WARNINGS_AS_ERRORS OFF)
add_subdirectory("${_kes_rmlui_dir}" "${CMAKE_BINARY_DIR}/external/RmlUi" EXCLUDE_FROM_ALL)

set(BUILD_SHARED_LIBS "${_kes_saved_build_shared_libs}")
unset(CMAKE_DISABLE_FIND_PACKAGE_Freetype)

foreach(_kes_ui_target IN ITEMS freetype rmlui_core rmlui_debugger)
    if(TARGET ${_kes_ui_target})
        set_target_properties(${_kes_ui_target} PROPERTIES FOLDER "External/UI")
    endif()
endforeach()

set(KES_RMLUI_BACKENDS_DIR "${_kes_rmlui_dir}/Backends")
