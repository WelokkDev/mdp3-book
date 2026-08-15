cmake_minimum_required(VERSION 3.25)

project(bookreplay
  VERSION 0.1.0
  DESCRIPTION "CME MDP 3.0 order book reconstruction from Databento DBN"
  LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BOOKREPLAY_IS_TOP_LEVEL ON)
else()
  set(BOOKREPLAY_IS_TOP_LEVEL OFF)
endif()

option(BOOKREPLAY_BUILD_TESTS "Build the test suite" ${BOOKREPLAY_IS_TOP_LEVEL})
option(BOOKREPLAY_BUILD_TOOLS "Build the command-line tools" ${BOOKREPLAY_IS_TOP_LEVEL})
option(BOOKREPLAY_BUILD_ORACLE "Build the databento-cpp differential oracle (requires an installed databento-cpp)" OFF)
option(BOOKREPLAY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the Ninja and Makefile generators emit compile_commands.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BOOKREPLAY_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Zstd)

add_library(bookreplay
  src/dbn.cpp
  src/dbn_reader.cpp
)
add_library(bookreplay::bookreplay ALIAS bookreplay)

target_include_directories(bookreplay
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(bookreplay PRIVATE bookreplay::zstd)
target_compile_features(bookreplay PUBLIC cxx_std_20)
bookreplay_set_warnings(bookreplay)

if(BOOKREPLAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BOOKREPLAY_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(BOOKREPLAY_BUILD_ORACLE)
  add_subdirectory(tests/oracle)
endif()
