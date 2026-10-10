add_library(hammurabi_wire STATIC codec.cpp)

target_include_directories(hammurabi_wire PUBLIC ${PROJECT_SOURCE_DIR}/include)

hammurabi_target_warnings(hammurabi_wire)
hammurabi_target_sanitizers(hammurabi_wire)
hammurabi_target_clang_tidy(hammurabi_wire)
