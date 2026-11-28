# Compressonator CMP_Core (vendored subset) for BC1-BC7 encode/decode.
# Also included by the KES Editor's standalone build, hence the list-relative path.

get_filename_component(_kes_cmp_dir "${CMAKE_CURRENT_LIST_DIR}/../external/compressonator" ABSOLUTE)
if(NOT EXISTS "${_kes_cmp_dir}/cmp_core/source/cmp_core.h")
    message(FATAL_ERROR "KES: external/compressonator is missing.")
endif()

set(_kes_cmp_core "${_kes_cmp_dir}/cmp_core")
set(_kes_cmp_math "${_kes_cmp_dir}/applications/_libs/cmp_math")

add_library(CMP_Core STATIC
    "${_kes_cmp_core}/shaders/bc1_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc2_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc3_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc4_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc5_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc6_encode_kernel.cpp"
    "${_kes_cmp_core}/shaders/bc7_encode_kernel.cpp"
    "${_kes_cmp_core}/source/cmp_core.cpp"
    "${_kes_cmp_math}/cpu_extensions.cpp"
    "${_kes_cmp_math}/cmp_math_common.cpp"
)
target_include_directories(CMP_Core SYSTEM PUBLIC "${_kes_cmp_core}/source")
target_include_directories(CMP_Core PRIVATE "${_kes_cmp_core}/shaders" "${_kes_cmp_math}")
set_target_properties(CMP_Core PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "external")
if(UNIX)
    target_compile_definitions(CMP_Core PRIVATE _LINUX ASPM_GPU)
endif()
if(MSVC)
    target_compile_options(CMP_Core PRIVATE /W0)
else()
    target_compile_options(CMP_Core PRIVATE -w)
endif()

# BC1 SIMD paths (x86 only), picked at runtime by EnableSSE4/AVX2/AVX512.
foreach(_simd SSE AVX AVX512)
    string(TOLOWER ${_simd} _simd_lower)
    add_library(CMP_Core_${_simd} STATIC "${_kes_cmp_core}/source/core_simd_${_simd_lower}.cpp")
    target_include_directories(CMP_Core_${_simd} PRIVATE "${_kes_cmp_core}/source" "${_kes_cmp_core}/shaders")
    set_target_properties(CMP_Core_${_simd} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "external")
    if(MSVC)
        target_compile_options(CMP_Core_${_simd} PRIVATE /W0)
    else()
        target_compile_options(CMP_Core_${_simd} PRIVATE -w)
    endif()
endforeach()
if(MSVC)
    target_compile_options(CMP_Core_AVX PRIVATE /arch:AVX2)
    target_compile_options(CMP_Core_AVX512 PRIVATE /arch:AVX512)
else()
    target_compile_options(CMP_Core_SSE PRIVATE -msse4.1)
    target_compile_options(CMP_Core_AVX PRIVATE -mavx2 -mfma)
    target_compile_options(CMP_Core_AVX512 PRIVATE -mavx512f -mavx512bw -mavx512dq -mavx512vl)
endif()
target_link_libraries(CMP_Core PRIVATE CMP_Core_SSE CMP_Core_AVX CMP_Core_AVX512)
