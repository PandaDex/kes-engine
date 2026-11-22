# Build-time packaging script, run by the kes_package target (see KesPackage.cmake).
#
# Produces ${OUTPUT_DIR}/${NAME}-${PLATFORM}${SUFFIX}/ :
#   <exe> + runtime libraries + crash handler (+ MSVC runtime with VC_RUNTIME_LIST)
#   config/             loose (players may edit input maps)
#   content/game.pak    game content (without sources) + compiled game shaders
#   content/engine.pak  engine assets + compiled engine shaders
#                       CONTENT_MODE=loose writes both as plain files under content/
#                       instead, the same layout as a development build
#   platform files      e.g. MicrosoftGame.config + logos for GDK
# then archives it according to FORMAT (zip, tar.gz, msixvc, folder). Symbols go by
# SYMBOLS_MODE: archive -> ${NAME}-${PLATFORM}${SUFFIX}-symbols-${VERSION}.zip (not
# shipped), package -> next to the binaries, both, or none.

foreach(_var PLATFORM NAME VERSION EXE EXE_DIR PAK_TOOL GAME_CONTENT_DIR ENGINE_ASSETS_DIR OUTPUT_DIR FORMAT)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "KesPackageStage: ${_var} not set")
    endif()
endforeach()

string(REPLACE "|" ";" RUNTIME_FILES "${RUNTIME_FILES}")
string(REPLACE "|" ";" PLATFORM_FILES "${PLATFORM_FILES}")
string(REPLACE "|" ";" CRASH_FILES "${CRASH_FILES}")
string(REPLACE "|" ";" SYMBOL_FILES "${SYMBOL_FILES}")
string(REPLACE "|" ";" SPLIT_LIBS "${SPLIT_LIBS}")
set(VC_RUNTIME_RELEASE "")
set(VC_RUNTIME_DEBUG "")
if(VC_RUNTIME_LIST)
    include("${VC_RUNTIME_LIST}")
endif()
if(NOT CONTENT_MODE)
    set(CONTENT_MODE "pak")
endif()
if(NOT SYMBOLS_MODE)
    set(SYMBOLS_MODE "archive")
endif()
if(NOT DEFINED STRIP_SOURCES OR "${STRIP_SOURCES}" STREQUAL "")
    set(STRIP_SOURCES ON)
endif()
set(_symbols_to_archive OFF)
set(_symbols_to_package OFF)
if(SYMBOLS_MODE MATCHES "^(archive|both)$")
    set(_symbols_to_archive ON)
endif()
if(SYMBOLS_MODE MATCHES "^(package|both)$")
    set(_symbols_to_package ON)
endif()

set(_package_name "${NAME}-${PLATFORM}${SUFFIX}")
set(_stage "${OUTPUT_DIR}/${_package_name}")
set(_work "${OUTPUT_DIR}/.work-${_package_name}")

message(STATUS "Staging ${_stage} (${CONFIG}, ${CONTENT_MODE} content, symbols: ${SYMBOLS_MODE})")
file(REMOVE_RECURSE "${_stage}" "${_work}")
file(MAKE_DIRECTORY "${_stage}/content" "${_work}")

# Executable and runtime libraries
file(COPY "${EXE}" DESTINATION "${_stage}")
foreach(_file IN LISTS RUNTIME_FILES)
    if(_file)
        file(COPY "${_file}" DESTINATION "${_stage}")
    endif()
endforeach()
if(NOT PLATFORM MATCHES "^(windows|gdk)$")
    # Linux: bundle_runtime_libs() already copied the shared libraries next to the exe.
    file(GLOB _shared_libs "${EXE_DIR}/*.so" "${EXE_DIR}/*.so.*")
    foreach(_lib IN LISTS _shared_libs)
        file(COPY "${_lib}" DESTINATION "${_stage}" FOLLOW_SYMLINK_CHAIN)
    endforeach()
endif()

foreach(_file IN LISTS CRASH_FILES)
    if(_file)
        file(COPY "${_file}" DESTINATION "${_stage}")
    endif()
endforeach()

if(CONFIG STREQUAL "Debug")
    set(_vc_runtime ${VC_RUNTIME_DEBUG})
else()
    set(_vc_runtime ${VC_RUNTIME_RELEASE})
endif()
foreach(_file IN LISTS _vc_runtime)
    if(_file AND EXISTS "${_file}")
        file(COPY "${_file}" DESTINATION "${_stage}")
    endif()
endforeach()

# Symbols, split from the shipped binaries unless they stay in the package
set(_symbols "${_work}/symbols")
file(MAKE_DIRECTORY "${_symbols}")
foreach(_file IN LISTS SYMBOL_FILES)
    if(_file AND EXISTS "${_file}")
        file(COPY "${_file}" DESTINATION "${_symbols}")
        if(_symbols_to_package)
            file(COPY "${_file}" DESTINATION "${_stage}")
        endif()
    endif()
endforeach()
if(NOT PLATFORM MATCHES "^(windows|gdk)$" AND OBJCOPY)
    function(_kes_split_debug binary)
        get_filename_component(_name "${binary}" NAME)
        set(_debug_file "${_symbols}/${_name}.debug")
        if(_symbols_to_archive)
            execute_process(COMMAND "${OBJCOPY}" --only-keep-debug "${binary}" "${_debug_file}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
        if(_symbols_to_package)
            return()
        endif()
        if(_symbols_to_archive)
            execute_process(COMMAND "${OBJCOPY}" --strip-debug "--add-gnu-debuglink=${_debug_file}" "${binary}" COMMAND_ERROR_IS_FATAL ANY)
        else()
            execute_process(COMMAND "${OBJCOPY}" --strip-debug "${binary}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endfunction()
    get_filename_component(_exe_name "${EXE}" NAME)
    _kes_split_debug("${_stage}/${_exe_name}")
    foreach(_lib_name IN LISTS SPLIT_LIBS)
        _kes_split_debug("${_stage}/${_lib_name}")
    endforeach()
    if(NOT _symbols_to_package)
        foreach(_file IN LISTS CRASH_FILES)
            get_filename_component(_handler_name "${_file}" NAME)
            execute_process(COMMAND "${OBJCOPY}" --strip-debug "${_stage}/${_handler_name}" COMMAND_ERROR_IS_FATAL ANY)
        endforeach()
    endif()
endif()
file(GLOB _symbol_entries RELATIVE "${_symbols}" "${_symbols}/*")
set(_symbols_archive "${OUTPUT_DIR}/${_package_name}-symbols-${VERSION}.zip")
file(REMOVE "${_symbols_archive}")
if(_symbols_to_archive AND _symbol_entries)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_symbols_archive}" --format=zip ${_symbol_entries}
        WORKING_DIRECTORY "${_symbols}"
        COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Symbols: ${_symbols_archive}")
endif()

# Content: game content plus compiled game shaders, engine assets (already rooted
# at engine/) plus compiled engine shaders.
set(_source_excludes "")
if(STRIP_SOURCES)
    set(_source_excludes PATTERN "*.slang" EXCLUDE PATTERN "*.slangnodes" EXCLUDE PATTERN "*.fmap" EXCLUDE)
endif()
file(COPY "${GAME_CONTENT_DIR}/" DESTINATION "${_work}/game" ${_source_excludes})
if(GAME_SHADERS_DIR AND EXISTS "${GAME_SHADERS_DIR}")
    file(COPY "${GAME_SHADERS_DIR}/" DESTINATION "${_work}/game")
endif()
file(COPY "${ENGINE_ASSETS_DIR}/" DESTINATION "${_work}/engine")
if(ENGINE_SHADERS_DIR AND EXISTS "${ENGINE_SHADERS_DIR}")
    file(COPY "${ENGINE_SHADERS_DIR}/" DESTINATION "${_work}/engine/engine/shaders")
endif()

if(CONTENT_MODE STREQUAL "loose")
    # The engine reads loose files under <exe>/content before any pak.
    file(COPY "${_work}/game/" DESTINATION "${_stage}/content")
    file(COPY "${_work}/engine/" DESTINATION "${_stage}/content")
    if(PAK_KEY)
        message(STATUS "Loose content: KES_PAK_ENCRYPTION_KEY has no effect on this package")
    endif()
    message(STATUS "Loose content in ${_stage}/content")
elseif(CONTENT_MODE STREQUAL "pak")
    set(_pak_args "")
    if(PAK_KEY)
        set(_pak_args --encrypt "${PAK_KEY}")
        message(STATUS "Paks will be encrypted")
    endif()

    function(_kes_make_pak source_dir pak_path)
        execute_process(
            COMMAND "${PAK_TOOL}" "${source_dir}" "${pak_path}" ${_pak_args}
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "pak-tool failed for ${pak_path}:\n${_out}${_err}")
        endif()
        message(STATUS "Packed ${pak_path}")
    endfunction()

    _kes_make_pak("${_work}/game" "${_stage}/content/game.pak")
    _kes_make_pak("${_work}/engine" "${_stage}/content/engine.pak")
else()
    message(FATAL_ERROR "Unknown content mode '${CONTENT_MODE}' (pak, loose)")
endif()

# Loose files
if(CONFIG_DIR AND EXISTS "${CONFIG_DIR}")
    file(COPY "${CONFIG_DIR}/" DESTINATION "${_stage}/config")
endif()
foreach(_file IN LISTS PLATFORM_FILES)
    if(_file)
        file(COPY "${_file}" DESTINATION "${_stage}")
    endif()
endforeach()

# Archive
if(FORMAT STREQUAL "zip")
    set(_archive "${OUTPUT_DIR}/${_package_name}-${VERSION}.zip")
    file(REMOVE "${_archive}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip "${_package_name}"
        WORKING_DIRECTORY "${OUTPUT_DIR}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Creating ${_archive} failed")
    endif()
    message(STATUS "Package: ${_archive}")

elseif(FORMAT STREQUAL "tar.gz")
    set(_archive "${OUTPUT_DIR}/${_package_name}-${VERSION}.tar.gz")
    file(REMOVE "${_archive}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar czf "${_archive}" "${_package_name}"
        WORKING_DIRECTORY "${OUTPUT_DIR}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Creating ${_archive} failed")
    endif()
    message(STATUS "Package: ${_archive}")

elseif(FORMAT STREQUAL "msixvc")
    if(NOT MAKEPKG OR NOT EXISTS "${MAKEPKG}")
        message(FATAL_ERROR "makepkg.exe not found (KES_GDK_MAKEPKG)")
    endif()
    set(_layout "${_work}/layout.xml")
    set(_msixvc_dir "${OUTPUT_DIR}/${_package_name}-msixvc")
    file(REMOVE_RECURSE "${_msixvc_dir}")
    file(MAKE_DIRECTORY "${_msixvc_dir}")

    execute_process(
        COMMAND "${MAKEPKG}" genmap /f "${_layout}" /d "${_stage}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "makepkg genmap failed")
    endif()
    execute_process(
        COMMAND "${MAKEPKG}" pack /f "${_layout}" /d "${_stage}" /pd "${_msixvc_dir}" /pc /loggable
                /symbolpaths "${EXE_DIR}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "makepkg pack failed (see output above)")
    endif()
    message(STATUS "Package: ${_msixvc_dir}")

elseif(FORMAT STREQUAL "folder")
    message(STATUS "Package: ${_stage}")
else()
    message(FATAL_ERROR "Unknown package format '${FORMAT}' (zip, tar.gz, msixvc, folder)")
endif()

file(REMOVE_RECURSE "${_work}")
