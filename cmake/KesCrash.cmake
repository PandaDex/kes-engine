# Crash reporting: sentry-native with its Crashpad backend (external/sentry-native,
# a git submodule). Crashes are captured by a separate crashpad_handler process, so
# stack overflows and heap corruption still produce a minidump. See docs/crash-reporting.md.
#
#   KES_CRASH_REPORTER   build it in (default ON, forced OFF with KES_SANITIZE)
#   KES_CRASH_DSN        where to upload reports; empty keeps them on the player's machine
#
# Provides kes_target_crash_reporter(), kes_deploy_crash_handler() and
# KES_CRASH_RUNTIME_FILES / KES_CRASH_RUNTIME_TARGETS for packaging.

include_guard(GLOBAL)

option(KES_CRASH_REPORTER "Capture crashes with Crashpad (minidumps, optional upload)" ON)
set(KES_CRASH_DSN "" CACHE STRING "Sentry-compatible DSN for crash uploads (empty = local minidumps only)")

# Release builds keep full debug info so shipped minidumps can be symbolicated.
# The PDBs and .debug files go into a separate symbols archive, never the game package.
if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Zi>" "$<$<CONFIG:Release>:/FS>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
else()
    add_compile_options("$<$<CONFIG:Release>:-g>")
    add_link_options("-Wl,--build-id")
endif()

set(_kes_crash_enabled ${KES_CRASH_REPORTER})
if(_kes_crash_enabled AND KES_SANITIZE)
    message(STATUS "KES: crash reporter disabled (ASan owns the crash signals)")
    set(_kes_crash_enabled OFF)
endif()

if(_kes_crash_enabled AND UNIX)
    find_package(CURL QUIET)
    find_package(ZLIB QUIET)
    if(NOT CURL_FOUND OR NOT ZLIB_FOUND)
        message(WARNING "KES: crash reporter disabled, it needs the libcurl and zlib development packages")
        set(_kes_crash_enabled OFF)
    endif()
endif()

set(KES_CRASH_RUNTIME_FILES "")
set(KES_CRASH_RUNTIME_TARGETS "")

if(_kes_crash_enabled)
    set(_kes_sentry_dir "${CMAKE_SOURCE_DIR}/external/sentry-native")
    if(NOT EXISTS "${_kes_sentry_dir}/external/crashpad/CMakeLists.txt")
        message(FATAL_ERROR "KES: crash reporter dependency missing: ${_kes_sentry_dir}\n"
            "Fetch it: git submodule update --init --recursive external/sentry-native\n"
            "or configure with -DKES_CRASH_REPORTER=OFF")
    endif()

    set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(UNIX)
        # Minidumps upload from crashpad_handler, which loads libcurl at runtime.
        # sentry's own curl transport would link the game to libcurl and bundle
        # its whole TLS stack into the package, just for non-crash events.
        set(SENTRY_TRANSPORT "none" CACHE STRING "" FORCE)
    endif()
    add_subdirectory("${_kes_sentry_dir}" "${CMAKE_BINARY_DIR}/external/sentry-native" EXCLUDE_FROM_ALL)

    if(UNIX)
        set_target_properties(crashpad_handler PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "\$ORIGIN")
    endif()

    list(APPEND KES_CRASH_RUNTIME_TARGETS crashpad_handler)
    list(APPEND KES_CRASH_RUNTIME_FILES "$<TARGET_FILE:crashpad_handler>")
    if(WIN32)
        # Lets Windows Error Reporting hand fail-fast crashes (stack cookie
        # failures, heap corruption) to the handler.
        list(APPEND KES_CRASH_RUNTIME_TARGETS crashpad_wer)
        list(APPEND KES_CRASH_RUNTIME_FILES "$<TARGET_FILE:crashpad_wer>")
    endif()
endif()

function(kes_target_crash_reporter target)
    if(_kes_crash_enabled)
        target_link_libraries(${target} PRIVATE sentry::sentry)
    endif()
endfunction()

# The handler has to sit next to the executable.
function(kes_deploy_crash_handler target)
    if(NOT _kes_crash_enabled)
        return()
    endif()
    add_dependencies(${target} ${KES_CRASH_RUNTIME_TARGETS})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${KES_CRASH_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying crash handler for ${target}"
        VERBATIM)
endfunction()
