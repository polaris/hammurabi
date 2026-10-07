cmake_minimum_required(VERSION 3.21)

project(hammurabi VERSION 0.1.0 LANGUAGES CXX)

set(HAMMURABI_SANITIZERS "" CACHE STRING "Sanitizers to build with, for example address,undefined or thread")
option(HAMMURABI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(HammurabiTargetOptions)

find_package(asio REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Catch2 3 REQUIRED)

enable_testing()

add_subdirectory(hammurabi)
add_subdirectory(server)
add_subdirectory(tests)
