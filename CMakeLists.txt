cmake_minimum_required(VERSION 3.23)

project(hammurabi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(HAMMURABI_SANITIZERS "" CACHE STRING "Sanitizers to build with, for example address,undefined or thread")
option(HAMMURABI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(HAMMURABI_CLANG_TIDY "Run clang-tidy while compiling" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(HammurabiTargetOptions)

find_package(asio REQUIRED)
find_package(CLI11 REQUIRED)

include(CTest)

# project(VERSION) is the single source of truth for the version; code reads it from <hammurabi/version.h>.
configure_file(cmake/version.h.in "${PROJECT_BINARY_DIR}/include/hammurabi/version.h" @ONLY)
add_library(hammurabi_version INTERFACE)
add_library(hammurabi::version ALIAS hammurabi_version)
target_include_directories(hammurabi_version INTERFACE "${PROJECT_BINARY_DIR}/include")

add_subdirectory(apps/cli)
add_subdirectory(apps/server)
add_subdirectory(src/wire)

if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests/unit)
endif()
