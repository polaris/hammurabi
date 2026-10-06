add_library(hammurabi STATIC connector.cpp kv_store.cpp raft.cpp)

protobuf_generate(
    TARGET hammurabi
    PROTOS ${PROJECT_SOURCE_DIR}/proto/raft.proto
    IMPORT_DIRS ${PROJECT_SOURCE_DIR}
)

target_compile_features(hammurabi PUBLIC cxx_std_23)
target_include_directories(hammurabi PUBLIC ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(hammurabi PUBLIC asio::asio protobuf::libprotobuf)
