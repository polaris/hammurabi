cmake_minimum_required(VERSION 3.13)

project(hammurabi)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Boost 1.68.0 REQUIRED COMPONENTS system filesystem)
find_package(Protobuf REQUIRED)
find_package(Catch2 REQUIRED)

add_library(hammurabi STATIC src/connector.cpp include/hammurabi/connector.h src/deadline_timer.cpp include/hammurabi/deadline_timer.h src/kv_store.cpp include/hammurabi/kv_store.h
        src/raft.cpp include/hammurabi/raft.h include/hammurabi/detail/rng.h include/hammurabi/types.h include/hammurabi/detail/hashmap.h proto/raft.pb.cc proto/raft.pb.h)
target_include_directories(hammurabi SYSTEM PRIVATE ${Boost_INCLUDE_DIR} ${Protobuf_INCLUDE_DIRS})

add_executable(tests tests/tests.cpp)
target_include_directories(tests SYSTEM PRIVATE . ${Boost_INCLUDE_DIR})
target_link_libraries(tests PRIVATE hammurabi ${Boost_LIBRARIES} ${Protobuf_LIBRARIES} Catch2::Catch2)

add_executable(server src/main.cpp)
target_include_directories(server SYSTEM PRIVATE . ${Boost_INCLUDE_DIR})
target_link_libraries(server PRIVATE hammurabi ${Boost_LIBRARIES} ${Protobuf_LIBRARIES})
