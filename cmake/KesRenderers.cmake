# KES renderer backend configuration.
#
# Each platform has a list of renderer backends that get compiled in, in
# priority order. The first entry is the default; the others are runtime
# choices and automatic fallbacks if the default fails to initialize.
# A list with a single entry forces that renderer on the platform (players and
# game code cannot select another one).
#
#   cmake -DKES_RENDERERS_WINDOWS="d3d11;vulkan" ..   # D3D11 default, Vulkan optional
#   cmake -DKES_RENDERERS_WINDOWS="vulkan" ..         # force Vulkan on Windows
#
# Known backends: vulkan, d3d11 (Windows and GDK only).
# The platform comes from KES_PLATFORM (cmake/KesPlatform.cmake).

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/KesPlatform.cmake")

set(KES_RENDERERS_WINDOWS "vulkan;d3d11" CACHE STRING "Renderer backends built on Windows, in priority order (vulkan, d3d11). One entry forces it.")
set(KES_RENDERERS_GDK     "d3d11;vulkan" CACHE STRING "Renderer backends built for GDK (PC), in priority order (d3d11, vulkan). One entry forces it.")
set(KES_RENDERERS_LINUX   "vulkan"       CACHE STRING "Renderer backends built on Linux, in priority order (vulkan). One entry forces it.")
set(KES_RENDERERS_MACOS   "vulkan"       CACHE STRING "Renderer backends built on macOS, in priority order (vulkan). One entry forces it.")
option(KES_ALLOW_RENDERER_SELECTION "Allow choosing between compiled renderers at runtime (-renderer=, KES_RENDERER, AppConfig::renderer). OFF always uses the first one." ON)

set(_kes_platform "${KES_PLATFORM_ID}")
if(NOT DEFINED KES_RENDERERS_${_kes_platform})
    message(FATAL_ERROR "KES: no renderer list for platform ${KES_PLATFORM} (KES_RENDERERS_${_kes_platform}).")
endif()

set(KES_RENDERERS "")
foreach(_kes_backend IN LISTS KES_RENDERERS_${_kes_platform})
    string(TOLOWER "${_kes_backend}" _kes_backend)
    string(STRIP "${_kes_backend}" _kes_backend)
    if(_kes_backend STREQUAL "")
        continue()
    endif()
    if(NOT _kes_backend MATCHES "^(vulkan|d3d11)$")
        message(FATAL_ERROR "KES: unknown renderer '${_kes_backend}' in KES_RENDERERS_${_kes_platform}. Known: vulkan, d3d11.")
    endif()
    if(_kes_backend STREQUAL "d3d11" AND NOT KES_PLATFORM MATCHES "^(Windows|GDK)$")
        message(FATAL_ERROR "KES: the d3d11 renderer is only available on Windows and GDK (KES_RENDERERS_${_kes_platform}).")
    endif()
    list(APPEND KES_RENDERERS "${_kes_backend}")
endforeach()
list(REMOVE_DUPLICATES KES_RENDERERS)

if(NOT KES_RENDERERS)
    message(FATAL_ERROR "KES: KES_RENDERERS_${_kes_platform} is empty; at least one renderer is required.")
endif()

set(KES_RENDERER_VULKAN OFF)
set(KES_RENDERER_D3D11 OFF)
if("vulkan" IN_LIST KES_RENDERERS)
    set(KES_RENDERER_VULKAN ON)
endif()
if("d3d11" IN_LIST KES_RENDERERS)
    set(KES_RENDERER_D3D11 ON)
endif()

# Shader binary formats needed by the enabled backends (consumed by KesShaders.cmake).
set(KES_SHADER_FORMATS "")
if(KES_RENDERER_VULKAN)
    list(APPEND KES_SHADER_FORMATS "spirv")
endif()
if(KES_RENDERER_D3D11)
    list(APPEND KES_SHADER_FORMATS "dxbc")
endif()

list(LENGTH KES_RENDERERS _kes_renderer_count)
list(GET KES_RENDERERS 0 _kes_default_renderer)
if(_kes_renderer_count EQUAL 1 OR NOT KES_ALLOW_RENDERER_SELECTION)
    message(STATUS "KES renderers: ${KES_RENDERERS} (forced: ${_kes_default_renderer})")
else()
    message(STATUS "KES renderers: ${KES_RENDERERS} (default: ${_kes_default_renderer}, selectable at runtime)")
endif()
