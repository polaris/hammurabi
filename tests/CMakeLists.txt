include(Catch)

add_executable(tests tests.cpp)
target_compile_features(tests PRIVATE cxx_std_20)
target_link_libraries(tests PRIVATE hammurabi Catch2::Catch2WithMain)

catch_discover_tests(tests)
