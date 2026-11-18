cmake_minimum_required(VERSION 3.16)
project(MiniContainer VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "MiniContainer uses Linux namespaces and cgroups; it only builds on Linux.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(minicontainer_core STATIC
  src/common/util.cpp
  src/common/registry.cpp
  src/resource/resource_manager.cpp
  src/namespace/namespace_manager.cpp
  src/filesystem/filesystem_manager.cpp
  src/process/process_manager.cpp
  src/container/container.cpp
  src/cli/cli.cpp)
target_include_directories(minicontainer_core PUBLIC include)

add_executable(minicontainer src/main.cpp)
target_link_libraries(minicontainer PRIVATE minicontainer_core)

option(MC_BUILD_TESTS "Build unit/integration tests" ON)
if(MC_BUILD_TESTS)
  enable_testing()
  foreach(t namespace resource filesystem container)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE minicontainer_core)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  target_compile_definitions(test_container PRIVATE MC_BINARY="$<TARGET_FILE:minicontainer>")
endif()
