cmake_minimum_required(VERSION 3.20)
project(MicroFS VERSION 0.1.0 LANGUAGES CXX)

option(MICROFS_BUILD_TESTS "Build MicroFS tests" ON)

find_package(Threads REQUIRED)

add_library(microfs
  src/block.cc
  src/block_cache.cc
  src/bloom_filter.cc
  src/env.cc
  src/table.cc
  src/wal.cc
)
add_library(microfs::microfs ALIAS microfs)
target_include_directories(microfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(microfs PUBLIC cxx_std_20)
target_link_libraries(microfs PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(microfs PRIVATE /W4 /permissive-)
else()
  target_compile_options(microfs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MICROFS_BUILD_TESTS)
  enable_testing()
  add_executable(microfs_core_tests tests/core_test.cc)
  target_link_libraries(microfs_core_tests PRIVATE microfs::microfs)
  if(NOT MSVC)
    target_compile_options(microfs_core_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME microfs_core_tests COMMAND microfs_core_tests)
endif()
