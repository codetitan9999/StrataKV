cmake_minimum_required(VERSION 3.20)

project(
  StrataKV
  VERSION 0.1.0
  DESCRIPTION "A C++ storage-engine project inspired by LSM-tree systems"
  LANGUAGES CXX
)

option(STRATAKV_BUILD_TESTS "Build StrataKV unit tests" ON)
option(STRATAKV_BUILD_BENCHMARKS "Build StrataKV benchmark harnesses" ON)

add_library(stratakv
  src/compaction.cc
  src/db.cc
  src/memtable.cc
  src/sstable.cc
  src/status.cc
  src/wal.cc
)

target_include_directories(stratakv
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(stratakv PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(stratakv PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(stratakv PRIVATE /W4)
endif()

if(STRATAKV_BUILD_TESTS)
  enable_testing()

  add_executable(stratakv_tests
    tests/db_test.cc
  )
  target_link_libraries(stratakv_tests PRIVATE stratakv)

  add_test(NAME stratakv_tests COMMAND stratakv_tests)
endif()

if(STRATAKV_BUILD_BENCHMARKS)
  add_executable(stratakv_kv_bench
    benchmarks/kv_bench.cc
  )
  target_link_libraries(stratakv_kv_bench PRIVATE stratakv)
endif()
