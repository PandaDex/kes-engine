# KES profiler (Tracy, external/tracy).
#
#   KES_PROFILING            OFF  master switch. ON builds Tracy's client into the
#                                 engine and defines KES_PROFILING=1.
#   KES_PROFILE_<CATEGORY>   ON   per-subsystem zones (only used with KES_PROFILING):
#                                 CORE INPUT WINDOW RENDER GPU WORLD ENTITIES
#                                 PHYSICS ASSETS AUDIO IO UI
#   KES_PROFILER_ON_DEMAND   OFF  ON: collect only while the Tracy viewer is
#                                 connected (startup/map load is missed unless you
#                                 connect first). OFF: data is buffered from launch.
#
# Example:
#   cmake -S . -B out/build/profile -DKES_PROFILING=ON -DKES_PROFILE_ENTITIES=OFF
#
# Connect with the Tracy viewer (tracy-profiler) built from the same Tracy
# version as external/tracy (see external/tracy/public/common/TracyVersion.hpp).

include_guard(GLOBAL)

option(KES_PROFILING "Build with the Tracy profiler (external/tracy)" OFF)
option(KES_PROFILER_ON_DEMAND "Tracy: only collect while a viewer is connected" OFF)

set(KES_PROFILE_CATEGORIES CORE INPUT WINDOW RENDER GPU WORLD ENTITIES PHYSICS ASSETS AUDIO IO UI)
foreach(_kes_cat IN LISTS KES_PROFILE_CATEGORIES)
    option(KES_PROFILE_${_kes_cat} "Profiler zones for ${_kes_cat} (needs KES_PROFILING)" ON)
endforeach()

if(KES_PROFILING)
    set(_kes_tracy_dir "${CMAKE_SOURCE_DIR}/external/tracy")
    if(NOT EXISTS "${_kes_tracy_dir}/CMakeLists.txt")
        message(FATAL_ERROR "KES: KES_PROFILING=ON but Tracy was not found at ${_kes_tracy_dir}")
    endif()

    # Tracy's options are plain option() calls; normal variables set here win (CMP0077).
    set(TRACY_ENABLE ON)
    set(TRACY_ON_DEMAND ${KES_PROFILER_ON_DEMAND})
    set(TRACY_STATIC ON)
    set(TRACY_ONLY_LOCALHOST ON)   # don't listen on the LAN by default
    add_subdirectory("${_kes_tracy_dir}" "${CMAKE_BINARY_DIR}/external/tracy" EXCLUDE_FROM_ALL)

    set(_kes_enabled_cats "")
    foreach(_kes_cat IN LISTS KES_PROFILE_CATEGORIES)
        if(KES_PROFILE_${_kes_cat})
            list(APPEND _kes_enabled_cats ${_kes_cat})
        endif()
    endforeach()
    string(REPLACE ";" " " _kes_enabled_cats "${_kes_enabled_cats}")
    message(STATUS "KES: profiler ON (Tracy). Categories: ${_kes_enabled_cats}")
else()
    message(STATUS "KES: profiler OFF (-DKES_PROFILING=ON to enable Tracy)")
endif()

# Adds the profiler definitions (and Tracy, when enabled) to a target.
# PUBLIC so games and tools linking the engine see the same switches.
function(kes_target_profiler target)
    if(KES_PROFILING)
        target_link_libraries(${target} PUBLIC Tracy::TracyClient)
        target_compile_definitions(${target} PUBLIC KES_PROFILING=1)
        foreach(_cat IN LISTS KES_PROFILE_CATEGORIES)
            if(KES_PROFILE_${_cat})
                target_compile_definitions(${target} PUBLIC KES_PROFILE_${_cat}=1)
            else()
                target_compile_definitions(${target} PUBLIC KES_PROFILE_${_cat}=0)
            endif()
        endforeach()
    else()
        target_compile_definitions(${target} PUBLIC KES_PROFILING=0)
    endif()
endfunction()
