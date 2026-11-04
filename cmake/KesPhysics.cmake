# KES physics: Box3D (external/box3d), a git submodule built from source.
# Only engine/collision_world.cpp uses it; its headers stay out of the engine API.
#
#   git submodule update --init external/box3d

set(_kes_box3d_dir "${CMAKE_SOURCE_DIR}/external/box3d")
if(NOT EXISTS "${_kes_box3d_dir}/CMakeLists.txt")
    message(FATAL_ERROR "KES: external/box3d is missing. Fetch it: git submodule update --init external/box3d")
endif()

# Its samples, tests and benchmarks are only defined when it is the top-level project.
add_subdirectory("${_kes_box3d_dir}" "${CMAKE_BINARY_DIR}/external/box3d" EXCLUDE_FROM_ALL)

# b3LoadV (src/simd_sse2.h) reads 8 bytes of a 4-byte aligned float[3] through a
# double*. Fine on x86, but UBSan aborts on it. Other sanitizer checks stay on.
if(KES_SANITIZE AND NOT MSVC)
    target_compile_options(box3d PRIVATE -fno-sanitize=alignment)
endif()
