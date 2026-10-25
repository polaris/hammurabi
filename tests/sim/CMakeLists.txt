include(Catch)

add_library(hammurabi_sim STATIC cluster.cpp)

target_include_directories(hammurabi_sim PUBLIC ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(hammurabi_sim PUBLIC hammurabi_raft)

hammurabi_target_warnings(hammurabi_sim)
hammurabi_target_sanitizers(hammurabi_sim)
hammurabi_target_clang_tidy(hammurabi_sim)

add_executable(sim_tests sim_tests.cpp)

target_link_libraries(sim_tests PRIVATE Catch2::Catch2WithMain hammurabi_sim)
hammurabi_target_warnings(sim_tests)
hammurabi_target_sanitizers(sim_tests)
hammurabi_target_clang_tidy(sim_tests)

catch_discover_tests(sim_tests ADD_TAGS_AS_LABELS)
