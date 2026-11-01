# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required(VERSION 3.21)

project ("kes")

function(bundle_runtime_libs target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    elseif(UNIX AND NOT APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "\$ORIGIN")
        # Use DT_RPATH (applies to transitive deps) instead of DT_RUNPATH
        target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DTARGET_FILE=$<TARGET_FILE:${target}>
                    -DOUT_DIR=$<TARGET_FILE_DIR:${target}>
                    -P "${CMAKE_SOURCE_DIR}/cmake/BundleRuntimeLibs.cmake")
    endif()
endfunction()

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(KesRenderers)   # KES_RENDERERS_<PLATFORM>: which renderer backends are built
include(KesShaders)     # Slang -> SPIR-V / DXBC at build time

# Include sub-projects.
add_subdirectory ("engine")
add_subdirectory ("game")
