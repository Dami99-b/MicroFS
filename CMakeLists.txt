cmake_minimum_required(VERSION 3.20)
project(MicroFS VERSION 0.1.0 LANGUAGES CXX)

option(MICROFS_BUILD_TESTS "Build MicroFS tests" ON)
option(MICROFS_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(MICROFS_BUILD_BENCHMARKS "Build reproducible benchmark executable" ON)

find_package(Threads REQUIRED)

add_library(microfs
  src/block.cc
  src/block_cache.cc
  src/bloom_filter.cc
  src/db.cc
  src/env.cc
  src/table.cc
  src/tick_store.cc
  src/version_set.cc
  src/wal.cc
)
add_library(microfs::microfs ALIAS microfs)
target_include_directories(microfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(microfs PUBLIC cxx_std_20)
target_link_libraries(microfs PUBLIC Threads::Threads)

add_executable(microfs_cli tools/microfs_cli.cc)
target_link_libraries(microfs_cli PRIVATE microfs::microfs)
if(NOT MSVC)
  target_compile_options(microfs_cli PRIVATE
    -Wall -Wextra -Wpedantic -Werror
  )
endif()

if(MICROFS_BUILD_BENCHMARKS)
  add_executable(microfs_bench bench/microfs_bench.cc)
  target_link_libraries(microfs_bench PRIVATE microfs::microfs)
  if(NOT MSVC)
    target_compile_options(microfs_bench PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
  endif()
endif()

if(MSVC)
  target_compile_options(microfs PRIVATE /W4 /permissive-)
else()
  target_compile_options(microfs PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MICROFS_BUILD_TESTS)
  enable_testing()
  add_executable(microfs_core_tests tests/core_test.cc)
  target_link_libraries(microfs_core_tests PRIVATE microfs::microfs)
  if(NOT MSVC)
    target_compile_options(microfs_core_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
  endif()

  if(MICROFS_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "MICROFS_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()
    target_compile_options(microfs PRIVATE
      -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
    )
    foreach(fuzzer IN ITEMS block wal table batch)
      add_executable(microfs_fuzz_${fuzzer} fuzz/fuzz_${fuzzer}.cc)
      target_link_libraries(microfs_fuzz_${fuzzer} PRIVATE microfs::microfs)
      target_compile_options(microfs_fuzz_${fuzzer} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
      )
      target_link_options(microfs_fuzz_${fuzzer} PRIVATE
        -fsanitize=fuzzer,address,undefined
      )
    endforeach()
  endif()
  add_test(NAME microfs_core_tests COMMAND microfs_core_tests)
endif()
