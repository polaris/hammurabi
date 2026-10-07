# Generated protobuf code lives in its own target so it is compiled without our warning flags.
# Its include directory is SYSTEM, so raft.pb.h does not trigger warnings where raft.h includes it.
add_library(hammurabi_proto STATIC)

protobuf_generate(
    TARGET hammurabi_proto
    PROTOS ${PROJECT_SOURCE_DIR}/proto/raft.proto
    IMPORT_DIRS ${PROJECT_SOURCE_DIR}
)

target_include_directories(hammurabi_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(hammurabi_proto PUBLIC protobuf::libprotobuf)
hammurabi_target_sanitizers(hammurabi_proto)

add_library(hammurabi STATIC connector.cpp kv_store.cpp raft.cpp)

target_compile_features(hammurabi PUBLIC cxx_std_23)
target_compile_definitions(hammurabi PUBLIC ASIO_NO_DEPRECATED)
target_include_directories(hammurabi PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hammurabi PUBLIC hammurabi_proto asio::asio)
hammurabi_target_warnings(hammurabi)
hammurabi_target_sanitizers(hammurabi)
hammurabi_target_clang_tidy(hammurabi)
