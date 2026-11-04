# Build checks: compiler warnings and sanitizers.
#
#   KES_WARNINGS_AS_ERRORS  treat warnings in KES targets as errors
#   KES_SANITIZE            address + undefined behavior sanitizers (ASan only on
#                           MSVC). Applies to every target, third-party included:
#                           instrumented std containers must match across libraries.

option(KES_WARNINGS_AS_ERRORS "Treat warnings in KES targets as errors" OFF)
option(KES_SANITIZE "Build with address and undefined behavior sanitizers" OFF)

if(KES_SANITIZE)
    if(MSVC)
        # /RTC checks can't be combined with ASan.
        foreach(_kes_flags CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
            string(REGEX REPLACE "/RTC[1csu]*" "" ${_kes_flags} "${${_kes_flags}}")
        endforeach()
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Makes the narrow Windows APIs and std::filesystem conversions use UTF-8, so
# install and user folders with any characters work (Windows 10 1903+).
function(kes_target_utf8 target)
    if(MSVC)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kes_utf8.manifest")
    endif()
endfunction()

function(kes_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /external:anglebrackets /external:W0)
        if(KES_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # Vulkan's "Info info = { sType }" idiom relies on zero-filled members.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
        if(KES_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
