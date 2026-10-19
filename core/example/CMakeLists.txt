# goldnet's core over plain UDP sockets, no Godot. macOS / Linux.
#
#   cmake -S core/example -B core/example/build
#   cmake --build core/example/build
#   core/example/build/goldnet_demo server
#   core/example/build/goldnet_demo client     # another terminal; run as many as you like
#
cmake_minimum_required(VERSION 3.22)
project(goldnet-core-demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(goldnet_demo demo.cpp)
target_include_directories(goldnet_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
