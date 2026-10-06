cmake_minimum_required(VERSION 3.21)

project(hammurabi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(asio REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Catch2 3 REQUIRED)

enable_testing()

add_subdirectory(tests)
add_subdirectory(server)
add_subdirectory(hammurabi)