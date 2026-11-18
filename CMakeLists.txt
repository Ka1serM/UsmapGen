cmake_minimum_required(VERSION 3.29)
project(UsmapGen LANGUAGES C CXX)

include(cmake/CompilerOptions.cmake)

option(USMAPGEN_BUILD_TOOLS "Build the command line tool" ${PROJECT_IS_TOP_LEVEL})
option(USMAPGEN_BUILD_TESTS "Build the regression tests" OFF)

add_subdirectory(external)
add_subdirectory(src)
if(USMAPGEN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(USMAPGEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
