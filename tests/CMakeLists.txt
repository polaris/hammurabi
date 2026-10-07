include(Catch)

add_executable(tests tests.cpp)
target_link_libraries(tests PRIVATE hammurabi Catch2::Catch2WithMain)
hammurabi_target_warnings(tests)
hammurabi_target_sanitizers(tests)
hammurabi_target_clang_tidy(tests)

catch_discover_tests(tests)
