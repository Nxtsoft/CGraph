cmake_minimum_required(VERSION 3.25)

project(cgraph_native
  VERSION 0.1.0
  DESCRIPTION "Native C++/C graph analysis engine with daemon and thin client"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(CGRAPH_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)
option(CGRAPH_BUILD_FUZZERS "Build libFuzzer targets" OFF)

if(MSVC)
  # Sources are UTF-8 (e.g. an em dash in src/cli/main.cpp); the CRT's POSIX
  # spellings (getenv, fopen) are used deliberately.
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CgraphWarnings)
include(CgraphSanitizers)

find_package(igraph CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

add_subdirectory(vendor/tree-sitter)
add_subdirectory(src/engine)
add_subdirectory(src/cli)
add_subdirectory(src/daemon)
add_subdirectory(src/client)
add_subdirectory(src/mcp)

if(BUILD_TESTING)
  add_subdirectory(tests/smoke)
endif()

if(CGRAPH_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
