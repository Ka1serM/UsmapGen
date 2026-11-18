cmake_minimum_required(VERSION 3.29)
project(UsmapGen VERSION 1.0.0 LANGUAGES C CXX)

include(cmake/CompilerOptions.cmake)

option(USMAPGEN_BUILD_TOOLS "Build the command line tool" ${PROJECT_IS_TOP_LEVEL})
option(USMAPGEN_BUILD_TESTS "Build the regression tests" ${PROJECT_IS_TOP_LEVEL})

add_subdirectory(external)
add_subdirectory(src)
if(USMAPGEN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
#! Emscripten has no file system for the test executables to read the dedicated servers from.
if(USMAPGEN_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()
