# meshoptimizer (external/meshoptimizer), a git submodule built from source.
# Only engine/kmodel_lod.cpp uses it (LOD generation); its headers stay out of the engine API.
#
#   git submodule update --init external/meshoptimizer

set(_kes_meshopt_dir "${CMAKE_SOURCE_DIR}/external/meshoptimizer")
if(NOT EXISTS "${_kes_meshopt_dir}/CMakeLists.txt")
    message(FATAL_ERROR "KES: external/meshoptimizer is missing. Fetch it: git submodule update --init external/meshoptimizer")
endif()

set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_kes_meshopt_dir}" "${CMAKE_BINARY_DIR}/external/meshoptimizer" EXCLUDE_FROM_ALL)
