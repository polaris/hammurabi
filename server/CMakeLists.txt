add_executable(server main.cpp)
target_link_libraries(server PRIVATE hammurabi)
hammurabi_target_warnings(server)
hammurabi_target_sanitizers(server)
