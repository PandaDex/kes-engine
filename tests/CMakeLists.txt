add_executable(kes_tests
    "test_main.cpp"
    "test_audio.cpp"
    "test_collision.cpp"
    "test_command_line.cpp"
    "test_entity_manager.cpp"
    "test_file_formats.cpp"
    "test_ini_file.cpp"
    "test_input.cpp"
    "test_logger.cpp"
    "test_map_file.cpp"
    "test_map_script.cpp"
    "test_pak.cpp"
    "test_physics_world.cpp"
    "test_scene.cpp"
    "test_text_parse.cpp"
)
set_property(TARGET kes_tests PROPERTY CXX_STANDARD 20)
target_link_libraries(kes_tests PRIVATE engine)
kes_target_warnings(kes_tests)
kes_target_utf8(kes_tests)
target_compile_definitions(kes_tests PRIVATE KES_TEST_CONTENT_DIRS="${CMAKE_SOURCE_DIR}/game/content|${CMAKE_SOURCE_DIR}/engine/assets")
bundle_runtime_libs(kes_tests)

add_test(NAME kes_tests COMMAND kes_tests)

if(TARGET sentry::sentry)
    add_executable(kes_crash_smoke "crash_smoke.cpp")
    set_property(TARGET kes_crash_smoke PROPERTY CXX_STANDARD 20)
    target_link_libraries(kes_crash_smoke PRIVATE engine)
    kes_target_warnings(kes_crash_smoke)
    bundle_runtime_libs(kes_crash_smoke)
    kes_deploy_crash_handler(kes_crash_smoke)
    add_test(NAME kes_crash_smoke COMMAND kes_crash_smoke)
endif()
