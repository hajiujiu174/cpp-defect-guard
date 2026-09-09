cmake_minimum_required(VERSION 3.20)
project(defectguard_vulnerable_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vulnerable_sample main.cpp)
enable_testing()
add_test(NAME sample_runs COMMAND vulnerable_sample safe)

