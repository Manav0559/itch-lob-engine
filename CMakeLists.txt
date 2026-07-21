cmake_minimum_required(VERSION 3.20)
project(itch_lob_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings are errors on OUR targets only — third-party code (Catch2) builds
# with its own flags so a toolchain bump never breaks the build on someone
# else's warnings.
set(PROJECT_WARNINGS -Wall -Wextra -Wpedantic -Werror)

add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Execution layer (TWAP/VWAP/POV) — header-only, same as itch. Twap/Vwap/Pov
# all live under include/exec/; unit_tests links this and instantiates them.
add_library(exec INTERFACE)
target_include_directories(exec INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(exec INTERFACE itch)

# ZLIB backs the gzip-streaming input path (include/io/gzip_source.hpp),
# needed by both the replay binary (.gz day files) and its unit test.
find_package(ZLIB REQUIRED)

# Threads backs the multi-threaded pipeline (pipeline/spsc_queue.hpp) and the
# multicast demo's background sender thread in tests.
find_package(Threads REQUIRED)

add_executable(replay src/replay_main.cpp)
target_link_libraries(replay PRIVATE itch ZLIB::ZLIB)
target_compile_options(replay PRIVATE ${PROJECT_WARNINGS})

# Comparison binary: same replay, decoupled into a parser thread and a
# book-builder thread joined by a lock-free SPSC queue instead of one thread
# doing both inline. replay.cpp itself is untouched aside from sharing its
# BookBuilder via pipeline/dispatch_to_book.hpp.
add_executable(replay_threaded src/replay_threaded_main.cpp)
target_link_libraries(replay_threaded PRIVATE itch ZLIB::ZLIB Threads::Threads)
target_compile_options(replay_threaded PRIVATE ${PROJECT_WARNINGS})

# UDP multicast feed handler + companion synthetic sender, running a real
# MoldUDP64 session (session header, sequence numbers, gap detection,
# retransmission-request gap-fill) — see
# include/net/multicast_receiver.hpp for the session-layer implementation.
add_executable(live_replay src/live_replay_main.cpp)
target_link_libraries(live_replay PRIVATE itch)
target_compile_options(live_replay PRIVATE ${PROJECT_WARNINGS})

add_executable(multicast_sender src/multicast_sender_main.cpp)
target_link_libraries(multicast_sender PRIVATE itch)
target_compile_options(multicast_sender PRIVATE ${PROJECT_WARNINGS})

# OrderBook vs LadderBook latency harness (see bench/README.md). Not part of
# `ctest` — it's a manual-run benchmark, not a correctness check — but it
# builds under the same warning-as-error bar as everything else.
add_executable(bench bench/bench_main.cpp)
target_link_libraries(bench PRIVATE itch ZLIB::ZLIB)
target_compile_options(bench PRIVATE ${PROJECT_WARNINGS})

# Single-threaded vs two-thread pipeline throughput/latency comparison (see
# bench/THREADED_PIPELINE_FINDINGS.md) — same manual-run, not-part-of-ctest
# treatment as `bench` above.
add_executable(bench_threaded bench/bench_threaded_main.cpp)
target_link_libraries(bench_threaded PRIVATE itch ZLIB::ZLIB Threads::Threads)
target_compile_options(bench_threaded PRIVATE ${PROJECT_WARNINGS})

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
add_executable(unit_tests
  tests/test_parser.cpp
  tests/test_book.cpp
  tests/test_ladder_book.cpp
  tests/test_exec_twap.cpp
  tests/test_exec_vwap.cpp
  tests/test_exec_pov.cpp
  tests/test_exec_almgren_chriss.cpp
  tests/test_io.cpp
  tests/test_fill_sim.cpp
  tests/test_full_day_invariants.cpp
  tests/test_spsc_queue.cpp
  tests/test_replay_threaded.cpp
  tests/test_multicast_receiver.cpp
  tests/test_moldudp64.cpp
  tests/test_risk_gate.cpp
  tests/test_thread_affinity.cpp
)
target_link_libraries(unit_tests PRIVATE itch exec ZLIB::ZLIB Threads::Threads
                                          Catch2::Catch2WithMain)
target_compile_options(unit_tests PRIVATE ${PROJECT_WARNINGS})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(unit_tests)

# The replay binary must always work end-to-end without a data file.
add_test(NAME replay_selftest COMMAND replay --selftest)
