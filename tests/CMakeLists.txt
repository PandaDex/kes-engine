add_executable(kes_tests
    "test_main.cpp"
    "test_collision.cpp"
    "test_command_line.cpp"
    "test_entity_manager.cpp"
    "test_file_formats.cpp"
    "test_ini_file.cpp"
    "test_logger.cpp"
    "test_map_file.cpp"
    "test_pak.cpp"
)
set_property(TARGET kes_tests PROPERTY CXX_STANDARD 20)
target_link_libraries(kes_tests PRIVATE engine)
kes_target_warnings(kes_tests)
bundle_runtime_libs(kes_tests)

add_test(NAME kes_tests COMMAND kes_tests)
