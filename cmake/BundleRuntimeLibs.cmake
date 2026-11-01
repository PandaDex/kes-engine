# Usage: cmake -DTARGET_FILE=<exe> -DOUT_DIR=<dir> -P BundleRuntimeLibs.cmake
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${TARGET_FILE}"
    RESOLVED_DEPENDENCIES_VAR deps
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES ${EXTRA_DIRS}
    PRE_EXCLUDE_REGEXES
        "^libc\\.so" "^libm\\.so" "^libdl\\.so" "^libpthread\\.so" "^librt\\.so"
        "^libresolv\\.so" "^libutil\\.so" "^ld-linux" "^linux-vdso"
        "^libgcc_s\\.so" "^libstdc\\+\\+\\.so"
        "^libvulkan\\.so"
)

foreach(dep IN LISTS deps)
    file(COPY "${dep}" DESTINATION "${OUT_DIR}" FOLLOW_SYMLINK_CHAIN)
endforeach()

if(unresolved)
    message(WARNING "Unresolved runtime dependencies: ${unresolved}")
endif()