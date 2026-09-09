cmake_minimum_required(VERSION 3.24)
project(codeguard_relations LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Analysis/compilation fixture only; never execute its recursive functions.
add_library(relations OBJECT main.cpp lib.cpp)
target_include_directories(relations PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(relations PRIVATE CODEGUARD_FIXTURE=1)
