cmake_minimum_required(VERSION 3.20)
# LLVM's dependency checks also compile C sources (for example, libffi).
project(defectguard_clang LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_executable(defectguard-clang main.cpp)
# Use builtin headers matching LibTooling even outside LLVM's bin directory.
find_program(DEFECTGUARD_CLANG_EXECUTABLE NAMES clang HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)
execute_process(COMMAND "${DEFECTGUARD_CLANG_EXECUTABLE}" -print-resource-dir
    OUTPUT_VARIABLE DEFECTGUARD_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
file(TO_CMAKE_PATH "${DEFECTGUARD_RESOURCE_DIR}" DEFECTGUARD_RESOURCE_DIR)
if(NOT EXISTS "${DEFECTGUARD_RESOURCE_DIR}/include/stddef.h")
    message(FATAL_ERROR "Clang builtin headers not found: ${DEFECTGUARD_RESOURCE_DIR}")
endif()
target_compile_definitions(defectguard-clang PRIVATE
    DEFECTGUARD_RESOURCE_DIR="${DEFECTGUARD_RESOURCE_DIR}")
target_include_directories(defectguard-clang SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
if(LLVM_DEFINITIONS)
    # LLVMConfig may export all -D flags as one space-separated string.
    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_options(defectguard-clang PRIVATE ${LLVM_DEFINITIONS_LIST})
endif()
target_link_libraries(
    defectguard-clang
    PRIVATE
    clangAST
    clangASTMatchers
    clangAnalysis
    clangBasic
    clangFrontend
    clangTooling
)
