cmake_minimum_required(VERSION 3.24)
project(CodeGuard VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
option(CODEGUARD_BUILD_GUI "Build the optional Qt 6 presentation layer" OFF)
option(CODEGUARD_BUILD_CLANG "Build the existing standalone LibTooling analyzer" OFF)
option(CODEGUARD_ENABLE_ANALYSIS "Enable Clang symbols, CFG metrics and project graphs in Core" OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
add_library(codeguard-core STATIC
    core/project/scanner.cpp
    core/project/configuration.cpp
    core/graph/graph.cpp
    core/query/query.cpp
    core/analyzer/rules.cpp
    core/process/process.cpp
    core/testing/engineering.cpp
    core/database/sqlite_database.cpp
    core/application.cpp)
target_include_directories(codeguard-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
target_link_libraries(codeguard-core PRIVATE SQLite3::SQLite3 Threads::Threads)
if(CODEGUARD_ENABLE_ANALYSIS)
    add_subdirectory(core/parser)
else()
    target_sources(codeguard-core PRIVATE core/parser/unavailable.cpp)
endif()
if(MSVC)
    target_compile_options(codeguard-core PRIVATE /W4 /utf-8)
else()
    target_compile_options(codeguard-core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(codeguard-cli cli/main.cpp)
target_link_libraries(codeguard-cli PRIVATE codeguard-core)
if(MINGW)
    target_link_options(codeguard-cli PRIVATE -municode)
endif()
if(CODEGUARD_BUILD_GUI)
    add_subdirectory(gui/qt)
endif()
if(CODEGUARD_BUILD_CLANG)
    add_subdirectory(native/clang_tool)
endif()
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests/codeguard)
endif()
