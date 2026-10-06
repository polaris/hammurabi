add_executable(server main.cpp)
target_compile_features(server PRIVATE cxx_std_20)
target_link_libraries(server PRIVATE hammurabi asio::asio)
