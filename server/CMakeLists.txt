add_executable(server main.cpp)
target_link_libraries(server PRIVATE hammurabi asio::asio)
