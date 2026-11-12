if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(model-importer-core LANGUAGES CXX)
endif()

add_library(model-import-core STATIC
    "fbx_importer.cpp"
    "gltf_importer.cpp"
    "model_import_api.cpp"
    "sound_import_api.cpp"
    "texture_import_api.cpp"
    "obj_importer.cpp"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET model-import-core PROPERTY CXX_STANDARD 20)
endif()

target_link_libraries(model-import-core PRIVATE engine windowscodecs ole32 mfplat mfreadwrite mfuuid)
target_include_directories(model-import-core PRIVATE
    "../../../external/cgltf"
)

# Command-line front end; the Forge editor runs it to import assets.
add_executable(model-importer
    "main.cpp"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET model-importer PROPERTY CXX_STANDARD 20)
endif()

target_link_libraries(model-importer PRIVATE model-import-core engine)
