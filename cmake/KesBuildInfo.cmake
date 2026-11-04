# Build-time script: writes kes_build_info.h for engine/build_info.cpp. Runs on
# every build so the commit stays current; the file only changes when a value does.
#
# Usage: cmake -DOUT=<header> -DSOURCE_DIR=<repo> -DGIT=<git> -DNAME=<game>
#              -DVERSION=<version> -DCONFIG=<config> -DPLATFORM=<platform> -P KesBuildInfo.cmake

set(KES_BUILD_COMMIT "")
if(GIT)
    execute_process(
        COMMAND "${GIT}" -C "${SOURCE_DIR}" describe --always --dirty --abbrev=10
        OUTPUT_VARIABLE KES_BUILD_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT KES_BUILD_COMMIT)
    set(KES_BUILD_COMMIT "unknown")
endif()

file(CONFIGURE OUTPUT "${OUT}" @ONLY CONTENT [[
#pragma once
#define KES_BUILD_NAME "@NAME@"
#define KES_BUILD_VERSION "@VERSION@"
#define KES_BUILD_COMMIT "@KES_BUILD_COMMIT@"
#define KES_BUILD_CONFIG "@CONFIG@"
#define KES_BUILD_PLATFORM "@PLATFORM@"
]])
