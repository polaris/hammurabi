cmake_minimum_required(VERSION 3.21)

project(hammurabi VERSION 0.1.0 LANGUAGES CXX)

find_package(asio REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Catch2 3 REQUIRED)

enable_testing()

add_subdirectory(hammurabi)
add_subdirectory(server)
add_subdirectory(tests)
