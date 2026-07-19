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

add_executable(replay src/replay_main.cpp)
target_link_libraries(replay PRIVATE itch ZLIB::ZLIB)
target_compile_options(replay PRIVATE ${PROJECT_WARNINGS})

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
  tests/test_io.cpp
)
target_link_libraries(unit_tests PRIVATE itch exec ZLIB::ZLIB Catch2::Catch2WithMain)
target_compile_options(unit_tests PRIVATE ${PROJECT_WARNINGS})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(unit_tests)

# The replay binary must always work end-to-end without a data file.
add_test(NAME replay_selftest COMMAND replay --selftest)
