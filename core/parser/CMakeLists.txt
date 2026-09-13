find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
    message(FATAL_ERROR "The Core PPCallbacks adapter currently targets LLVM/Clang 22.x; use matching development packages")
endif()
find_program(CODEGUARD_CLANG_EXECUTABLE NAMES clang HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH REQUIRED)
execute_process(COMMAND "${CODEGUARD_CLANG_EXECUTABLE}" -print-resource-dir
    OUTPUT_VARIABLE CODEGUARD_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(TO_CMAKE_PATH "${CODEGUARD_RESOURCE_DIR}" CODEGUARD_RESOURCE_DIR)
if(NOT EXISTS "${CODEGUARD_RESOURCE_DIR}/include/stddef.h")
    message(FATAL_ERROR "Missing matching Clang builtin headers")
endif()
target_sources(codeguard-core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/clang_analysis.cpp")
foreach(rule engine unbounded_call array_bounds assignment_condition local_lifetime null_dereference)
    target_sources(codeguard-core PRIVATE "${PROJECT_SOURCE_DIR}/core/analyzer/clang_rules/${rule}.cpp")
endforeach()
target_include_directories(codeguard-core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(codeguard-core PRIVATE CODEGUARD_RESOURCE_DIR="${CODEGUARD_RESOURCE_DIR}")

# Version the analyzer implementation independently of unrelated documentation,
# UI edits, checkout paths and line-ending conversion. Reconfigure on edits.
file(GLOB analyzer_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/core/analyzer/clang_rules/*.cpp"
    "${PROJECT_SOURCE_DIR}/core/analyzer/clang_rules/*.hpp")
list(APPEND analyzer_inputs "${CMAKE_CURRENT_SOURCE_DIR}/clang_analysis.cpp"
    "${PROJECT_SOURCE_DIR}/core/include/codeguard/analysis.hpp"
    "${PROJECT_SOURCE_DIR}/core/include/codeguard/rules.hpp"
    "${PROJECT_SOURCE_DIR}/core/analyzer/rules.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT analyzer_inputs)
set(analyzer_content "${LLVM_PACKAGE_VERSION};${PROJECT_VERSION}")
foreach(input IN LISTS analyzer_inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(READ "${input}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 source_hash "${source}")
    string(APPEND analyzer_content ";${source_hash}")
endforeach()
string(SHA256 analyzer_revision "${analyzer_content}")
target_compile_definitions(codeguard-core PRIVATE CODEGUARD_ANALYZER_REVISION="${analyzer_revision}")
target_link_libraries(codeguard-core PRIVATE clangAST clangASTMatchers clangAnalysis clangIndex clangFrontend clangTooling)
