# Build-time packaging script, run by the kes_package target (see KesPackage.cmake).
#
# Produces ${OUTPUT_DIR}/${NAME}-${PLATFORM}/ :
#   <exe> + runtime libraries + crash handler
#   config/             loose (players may edit input maps)
#   content/game.pak    game content (without .slang sources) + compiled game shaders
#   content/engine.pak  engine assets + compiled engine shaders
#   platform files      e.g. MicrosoftGame.config + logos for GDK
# then archives it according to FORMAT (zip, tar.gz, msixvc, folder) and writes
# ${NAME}-${PLATFORM}-symbols-${VERSION}.zip next to it, which is not shipped.

foreach(_var PLATFORM NAME VERSION EXE EXE_DIR PAK_TOOL GAME_CONTENT_DIR ENGINE_ASSETS_DIR OUTPUT_DIR FORMAT)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "KesPackageStage: ${_var} not set")
    endif()
endforeach()

string(REPLACE "|" ";" RUNTIME_FILES "${RUNTIME_FILES}")
string(REPLACE "|" ";" PLATFORM_FILES "${PLATFORM_FILES}")
string(REPLACE "|" ";" CRASH_FILES "${CRASH_FILES}")
string(REPLACE "|" ";" SYMBOL_FILES "${SYMBOL_FILES}")

set(_package_name "${NAME}-${PLATFORM}")
set(_stage "${OUTPUT_DIR}/${_package_name}")
set(_work "${OUTPUT_DIR}/.work-${PLATFORM}")

message(STATUS "Staging ${_stage} (${CONFIG})")
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

# Symbols, split from the shipped binaries
set(_symbols "${_work}/symbols")
file(MAKE_DIRECTORY "${_symbols}")
foreach(_file IN LISTS SYMBOL_FILES)
    if(_file)
        file(COPY "${_file}" DESTINATION "${_symbols}")
    endif()
endforeach()
if(NOT PLATFORM MATCHES "^(windows|gdk)$" AND OBJCOPY)
    get_filename_component(_exe_name "${EXE}" NAME)
    set(_debug_file "${_symbols}/${_exe_name}.debug")
    execute_process(COMMAND "${OBJCOPY}" --only-keep-debug "${_stage}/${_exe_name}" "${_debug_file}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${OBJCOPY}" --strip-debug "--add-gnu-debuglink=${_debug_file}" "${_stage}/${_exe_name}" COMMAND_ERROR_IS_FATAL ANY)
    foreach(_file IN LISTS CRASH_FILES)
        get_filename_component(_handler_name "${_file}" NAME)
        execute_process(COMMAND "${OBJCOPY}" --strip-debug "${_stage}/${_handler_name}" COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
endif()
file(GLOB _symbol_entries RELATIVE "${_symbols}" "${_symbols}/*")
if(_symbol_entries)
    set(_symbols_archive "${OUTPUT_DIR}/${_package_name}-symbols-${VERSION}.zip")
    file(REMOVE "${_symbols_archive}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_symbols_archive}" --format=zip ${_symbol_entries}
        WORKING_DIRECTORY "${_symbols}"
        COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Symbols: ${_symbols_archive}")
endif()

# Paks
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

# Game: content minus shader sources, plus the compiled binaries.
file(COPY "${GAME_CONTENT_DIR}/" DESTINATION "${_work}/game" PATTERN "*.slang" EXCLUDE PATTERN "*.slangnodes" EXCLUDE PATTERN "*.fmap" EXCLUDE)
if(GAME_SHADERS_DIR AND EXISTS "${GAME_SHADERS_DIR}")
    file(COPY "${GAME_SHADERS_DIR}/" DESTINATION "${_work}/game")
endif()
_kes_make_pak("${_work}/game" "${_stage}/content/game.pak")

# Engine: engine/assets/* (already rooted at engine/) + compiled engine shaders.
file(COPY "${ENGINE_ASSETS_DIR}/" DESTINATION "${_work}/engine")
if(ENGINE_SHADERS_DIR AND EXISTS "${ENGINE_SHADERS_DIR}")
    file(COPY "${ENGINE_SHADERS_DIR}/" DESTINATION "${_work}/engine/engine/shaders")
endif()
_kes_make_pak("${_work}/engine" "${_stage}/content/engine.pak")

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
