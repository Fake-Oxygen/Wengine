cmake_minimum_required(VERSION 3.20)

project(WEngine
    VERSION 1.0.0
    DESCRIPTION "High-Performance C++20 Physics Simulation Engine"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are disabled. Use 'cmake -S . -B build'.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_CLI "Build CLI app" ON)
option(BUILD_TESTS "Build lightweight tests (no external deps)" ON)
option(BUILD_BENCHMARKS "Build micro-benchmarks" ON)

# ------------------------------------------------------------------
# Header-only engine modules: include/wengine/<module>/*.hpp
# Convention: one INTERFACE target per module, snake_case files,
# PascalCase types, wengine::<module> namespaces, no I/O in solvers.
# ------------------------------------------------------------------
add_library(wengine_math INTERFACE)
target_include_directories(wengine_math INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(wengine_units INTERFACE)
target_include_directories(wengine_units INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(wengine_solvers INTERFACE)
target_include_directories(wengine_solvers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_solvers INTERFACE wengine_math wengine_units)

add_library(wengine_world INTERFACE)
target_include_directories(wengine_world INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(wengine_emit INTERFACE)
target_include_directories(wengine_emit INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_emit INTERFACE wengine_world)

add_library(wengine_core INTERFACE)
target_link_libraries(wengine_core INTERFACE
    wengine_math wengine_units wengine_solvers wengine_world wengine_emit
)
add_library(wengine::core ALIAS wengine_core)

# Legacy alias so old links keep working during migration.
add_library(PhysicsEngineCore ALIAS wengine_core)

# ------------------------------------------------------------------
# CLI: thin adapter only (src/cli/*). No physics here, calls solvers.
# Future GUI (apps/gui) links wengine_core the same way, never the CLI.
# ------------------------------------------------------------------
if(BUILD_CLI)
    add_library(wengine_cli STATIC
        src/cli/shell.cpp
        src/cli/commands/ohm.cpp
    )
    target_include_directories(wengine_cli PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(wengine_cli PUBLIC wengine_core)

    target_compile_options(wengine_cli PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

    add_executable(PhysicsSimApp apps/cli/main.cpp)
    target_link_libraries(PhysicsSimApp PRIVATE wengine_cli)
endif()

# ------------------------------------------------------------------
# Tests: dependency-free asserts runnable via ctest.
# ------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_ohm_law tests/test_ohm_law.cpp)
    target_link_libraries(test_ohm_law PRIVATE wengine_core)
    add_test(NAME ohm_law COMMAND test_ohm_law)

    add_executable(test_math_units tests/test_math_units.cpp)
    target_link_libraries(test_math_units PRIVATE wengine_core)
    add_test(NAME math_units COMMAND test_math_units)

    add_executable(test_cli_args tests/test_cli_args.cpp)
    target_link_libraries(test_cli_args PRIVATE wengine_cli)
    add_test(NAME cli_args COMMAND test_cli_args)
endif()

if(BUILD_BENCHMARKS)
    add_executable(bench_ohm benchmarks/bench_ohm.cpp)
    target_link_libraries(bench_ohm PRIVATE wengine_core)
endif()
