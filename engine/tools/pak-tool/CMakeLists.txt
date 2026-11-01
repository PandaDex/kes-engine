if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(pak-tool LANGUAGES CXX)
endif()

add_executable(pak-tool
    "main.cpp"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET pak-tool PROPERTY CXX_STANDARD 20)
endif()

target_link_libraries(pak-tool PRIVATE engine)

add_custom_command(TARGET pak-tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:game>/content"
    COMMAND $<TARGET_FILE:pak-tool>
        "${CMAKE_SOURCE_DIR}/engine/assets"
        "$<TARGET_FILE_DIR:game>/content/engine.pak"
)