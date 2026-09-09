cmake_minimum_required(VERSION 3.20)
project(defectguard_ml_corpus LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(GLOB_RECURSE CORPUS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/case*/*.cpp")
# Only compile: positive samples intentionally contain undefined behavior. Never execute them.
add_library(defectguard_ml_corpus OBJECT ${CORPUS_SOURCES})
