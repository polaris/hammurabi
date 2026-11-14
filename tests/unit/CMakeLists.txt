include(Catch)

add_executable(unit_tests codec_tests.cpp
                          raft_messages_tests.cpp
                          raft_codec_tests.cpp
                          raft_node_tests.cpp
                          kv_tests.cpp
                          protocol_tests.cpp
                          replica_tests.cpp)
# hammurabi_replica brings hammurabi_raft and hammurabi_kv with it; listing them again makes ld warn about duplicates.
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain hammurabi_replica)
hammurabi_target_warnings(unit_tests)
hammurabi_target_sanitizers(unit_tests)
hammurabi_target_clang_tidy(unit_tests)

catch_discover_tests(unit_tests ADD_TAGS_AS_LABELS)
