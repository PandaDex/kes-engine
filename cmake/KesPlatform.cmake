# KES target platform selection.
#
#   KES_PLATFORM   Windows | GDK | GDKX | Linux | macOS
#
#   Windows  plain Win32 desktop build (default on Windows hosts)
#   GDK      Microsoft GDK for PC (desktop). Win32 + Gaming Runtime (XGameRuntime),
#            MicrosoftGame.config next to the exe, MSIXVC packaging via makepkg.
#   GDKX     Microsoft GDK for Xbox consoles (not implemented yet) WILL REQUIRE ADDITIONAL NDA FOR DOWNLOAD
#   Linux    default on Linux hosts
#   macOS    not implemented yet STATUS: Sure why not but i dont have a mac
#
# Compile definitions (PUBLIC on the engine target):
#   Windows -> PLATFORM_WINDOWS=1
#   GDK     -> PLATFORM_WINDOWS=1 PLATFORM_GDK=1 _GAMING_DESKTOP
#   Linux   -> PLATFORM_LINUX=1
#
# GDK location: the GDK installer sets GameDKCoreLatest / GameDK. Override with
#   -DKES_GDK_ROOT="C:/Program Files (x86)/Microsoft GDK/260405"

include_guard(GLOBAL)

if(WIN32)
    set(_kes_default_platform "Windows")
    set(_kes_allowed_platforms Windows GDK GDKX)
elseif(APPLE)
    set(_kes_default_platform "macOS")
    set(_kes_allowed_platforms macOS)
elseif(UNIX)
    set(_kes_default_platform "Linux")
    set(_kes_allowed_platforms Linux)
else()
    message(FATAL_ERROR "KES: unsupported host system")
endif()

set(KES_PLATFORM "${_kes_default_platform}" CACHE STRING "Target platform (${_kes_allowed_platforms})")
set_property(CACHE KES_PLATFORM PROPERTY STRINGS ${_kes_allowed_platforms})

if(NOT KES_PLATFORM IN_LIST _kes_allowed_platforms)
    message(FATAL_ERROR "KES: KES_PLATFORM='${KES_PLATFORM}' is not valid on this host. Allowed: ${_kes_allowed_platforms}")
endif()

# Upper-case id used for per-platform cache variables (KES_RENDERERS_<ID>).
string(TOUPPER "${KES_PLATFORM}" KES_PLATFORM_ID)

set(KES_PLATFORM_DEFINITIONS "")
if(KES_PLATFORM STREQUAL "Windows")
    list(APPEND KES_PLATFORM_DEFINITIONS PLATFORM_WINDOWS=1)
elseif(KES_PLATFORM STREQUAL "GDK")
    list(APPEND KES_PLATFORM_DEFINITIONS PLATFORM_WINDOWS=1 PLATFORM_GDK=1 _GAMING_DESKTOP)
elseif(KES_PLATFORM STREQUAL "GDKX")
    message(FATAL_ERROR "KES: KES_PLATFORM=GDKX (Xbox consoles) is not implemented yet. Use GDK for the PC version.")
elseif(KES_PLATFORM STREQUAL "Linux")
    list(APPEND KES_PLATFORM_DEFINITIONS PLATFORM_LINUX=1)
elseif(KES_PLATFORM STREQUAL "macOS")
    message(FATAL_ERROR "KES: macOS is not implemented yet.")
endif()

# --- Microsoft GDK (PC) -----
if(KES_PLATFORM STREQUAL "GDK")
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "KES: the GDK platform only supports x64 builds.")
    endif()

    set(_kes_gdk_hint "")
    if(DEFINED ENV{GameDKCoreLatest})
        set(_kes_gdk_hint "$ENV{GameDKCoreLatest}")
    elseif(DEFINED ENV{GameDK})
        # Pick the newest edition folder (editions are named YYMMNN).
        file(GLOB _kes_gdk_editions LIST_DIRECTORIES true "$ENV{GameDK}/[0-9][0-9][0-9][0-9][0-9][0-9]")
        if(_kes_gdk_editions)
            list(SORT _kes_gdk_editions)
            list(GET _kes_gdk_editions -1 _kes_gdk_hint)
        endif()
    endif()
    file(TO_CMAKE_PATH "${_kes_gdk_hint}" _kes_gdk_hint)
    string(REGEX REPLACE "/+$" "" _kes_gdk_hint "${_kes_gdk_hint}")

    set(KES_GDK_ROOT "${_kes_gdk_hint}" CACHE PATH "Microsoft GDK edition folder, e.g. C:/Program Files (x86)/Microsoft GDK/260405")
    if(NOT KES_GDK_ROOT OR NOT EXISTS "${KES_GDK_ROOT}")
        message(FATAL_ERROR "KES: Microsoft GDK not found. Install the GDK (https://aka.ms/gdk) or set KES_GDK_ROOT to an edition folder.")
    endif()

    # GDK 2025.10+ uses a unified windows/ layout; older editions use GRDK/GameKit.
    if(EXISTS "${KES_GDK_ROOT}/windows/lib/x64/xgameruntime.lib")
        set(KES_GDK_INCLUDE_DIR "${KES_GDK_ROOT}/windows/include")
        set(KES_GDK_LIBRARY "${KES_GDK_ROOT}/windows/lib/x64/xgameruntime.lib")
    elseif(EXISTS "${KES_GDK_ROOT}/GRDK/GameKit/Lib/amd64/xgameruntime.lib")
        set(KES_GDK_INCLUDE_DIR "${KES_GDK_ROOT}/GRDK/GameKit/Include")
        set(KES_GDK_LIBRARY "${KES_GDK_ROOT}/GRDK/GameKit/Lib/amd64/xgameruntime.lib")
    else()
        message(FATAL_ERROR "KES: xgameruntime.lib not found under ${KES_GDK_ROOT} (looked in windows/lib/x64 and GRDK/GameKit/Lib/amd64).")
    endif()

    get_filename_component(_kes_gdk_parent "${KES_GDK_ROOT}" DIRECTORY)
    find_program(KES_GDK_MAKEPKG makepkg
        HINTS "${_kes_gdk_parent}/bin" "${KES_GDK_ROOT}/bin" "${KES_GDK_ROOT}/windows/bin/x64"
        DOC "GDK makepkg.exe used to build MSIXVC packages")

    if(NOT TARGET Kes::GDK)
        add_library(Kes::GDK INTERFACE IMPORTED)
        set_target_properties(Kes::GDK PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${KES_GDK_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${KES_GDK_LIBRARY}")
    endif()

    get_filename_component(_kes_gdk_edition "${KES_GDK_ROOT}" NAME)
    message(STATUS "KES platform: GDK (PC), edition ${_kes_gdk_edition}")
    if(NOT KES_GDK_MAKEPKG)
        message(WARNING "KES: makepkg.exe not found; MSIXVC packaging will be unavailable.")
    endif()
else()
    message(STATUS "KES platform: ${KES_PLATFORM}")
endif()
