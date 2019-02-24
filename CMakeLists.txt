cmake_minimum_required(VERSION 3.13)

project(hammurabi)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Boost 1.68.0 REQUIRED COMPONENTS system filesystem)
find_package(Protobuf REQUIRED)
find_package(Catch2 REQUIRED)

add_library(hammurabi STATIC connector.cpp connector.h deadline_timer.cpp deadline_timer.h kv_store.cpp kv_store.h
        raft.cpp raft.h detail/rng.h types.h detail/hashmap.cpp detail/hashmap.h)
target_include_directories(hammurabi SYSTEM PRIVATE ${Boost_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})

add_executable(tests tests.cpp)
target_include_directories(tests SYSTEM PRIVATE . ${Boost_INCLUDE_DIR})
target_link_libraries(tests PRIVATE hammurabi ${Boost_LIBRARIES} ${Protobuf_LIBRARIES} Catch2::Catch2)

add_executable(server main.cpp)
target_include_directories(server SYSTEM PRIVATE . ${Boost_INCLUDE_DIR})
target_link_libraries(server PRIVATE hammurabi ${Boost_LIBRARIES} ${Protobuf_LIBRARIES})