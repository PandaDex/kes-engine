cmake_minimum_required(VERSION 3.21)
project(KesTools LANGUAGES CXX)

set(KES_TOOLS_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin" CACHE PATH "Where built tool executables are collected")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

file(GLOB KES_TOOL_DIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
set(KES_TOOL_EXECUTABLES)

foreach(dir IN LISTS KES_TOOL_DIRS)
    if(NOT IS_DIRECTORY "${dir}" OR NOT EXISTS "${dir}/CMakeLists.txt")
        continue()
    endif()
    add_subdirectory("${dir}")

    # Collect executables from the tool and its nested directories, skipping tests
    set(pending "${dir}")
    while(pending)
        list(POP_FRONT pending current)
        get_property(targets DIRECTORY "${current}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "EXECUTABLE" AND NOT target MATCHES "(^|_)tests?($|_)")
                list(APPEND KES_TOOL_EXECUTABLES ${target})
            endif()
        endforeach()
        get_property(subdirs DIRECTORY "${current}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirs})
    endwhile()
endforeach()

message(STATUS "KES tools copied to ${KES_TOOLS_BIN_DIR}: ${KES_TOOL_EXECUTABLES}")

# One target does all copying so parallel builds never race on the same files in bin
set(copy_commands)
set(deployed_files)
foreach(target IN LISTS KES_TOOL_EXECUTABLES)
    list(APPEND copy_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "${KES_TOOLS_BIN_DIR}")
    list(APPEND deployed_files "${KES_TOOLS_BIN_DIR}/$<TARGET_FILE_NAME:${target}>")
endforeach()

if(TARGET forge)
    list(APPEND copy_commands COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/forgeMapEditor/entities" "${KES_TOOLS_BIN_DIR}/entities")
endif()

if(WIN32)
    find_program(KES_WINDEPLOYQT windeployqt6 windeployqt HINTS "${Qt6_DIR}/../../../bin" NO_DEFAULT_PATH)
    if(KES_WINDEPLOYQT)
        list(APPEND copy_commands COMMAND "${KES_WINDEPLOYQT}" --no-compiler-runtime ${deployed_files})
    else()
        message(WARNING "windeployqt not found next to Qt; Qt DLLs will not be deployed to ${KES_TOOLS_BIN_DIR}")
    endif()
endif()

add_custom_target(kes_tools_bin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KES_TOOLS_BIN_DIR}"
    ${copy_commands}
    COMMENT "Copying tool executables to ${KES_TOOLS_BIN_DIR}"
    VERBATIM
)
add_dependencies(kes_tools_bin ${KES_TOOL_EXECUTABLES})
