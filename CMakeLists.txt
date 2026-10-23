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
option(BUILD_GUI "Build GUI app (scaffold, backend TBD per PHYSI-13)" OFF)
option(BUILD_ROS2 "Build ROS2 bridge (rclcpp, edge-only; OFF keeps builds ROS-free)" OFF)
option(BUILD_TUI "Build ncurses TUI for PhysicsSimApp (--tui flag, system libncurses)" ON)
option(BUILD_TESTS "Build lightweight tests (no external deps)" ON)
option(BUILD_BENCHMARKS "Build micro-benchmarks" ON)

# ------------------------------------------------------------------
# Test assertion policy: tests use plain assert(), often around
# operations with side effects. Default CMake Release/RelWithDebInfo
# defines NDEBUG, which would silently disable both the checks and
# those operations. Every CTest binary links wengine_test_assert,
# which undefines NDEBUG (-UNDEBUG on GCC/Clang, /UNDEBUG on MSVC)
# after any -DNDEBUG from the build type, so Debug and optimized
# test builds both execute assertions. Benchmarks never link this
# target (separate config, keeps its own NDEBUG/optimization).
# ------------------------------------------------------------------
add_library(wengine_test_assert INTERFACE)
target_compile_options(wengine_test_assert INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
)

# ------------------------------------------------------------------
# Header-only engine modules: include/wengine/<module>/*.hpp
# Convention: one INTERFACE target per module, snake_case files,
# PascalCase types, wengine::<module> namespaces, no I/O in solvers.
# ------------------------------------------------------------------
# GLM (external/glm submodule, pinned 1.0.1, MIT, header-only): the only
# third-party dependency, consumed directly as glm:: types (no wengine
# alias layer). Targets that need it link wengine_glm explicitly.
add_library(wengine_glm INTERFACE)
target_include_directories(wengine_glm INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/glm>
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
target_link_libraries(wengine_solvers INTERFACE wengine_units wengine_signal)

add_library(wengine_signal INTERFACE)
target_include_directories(wengine_signal INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_signal INTERFACE wengine_units)

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

# wengine-link wire protocol (PHYSI wengine-link epic): versioned seqlock
# snapshot bus types + pure encode/decode. Header-only, OS-free; transport
# endpoints (SHM/TCP publishers) attach as emit::ISink at the app edge.
add_library(wengine_link INTERFACE)
target_include_directories(wengine_link INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_link INTERFACE wengine_world)

# wengine-link SHM endpoints (PHYSI wengine-link epic): POSIX shm_open/mmap
# publisher + reader as an emit::ISink / pull reader pair. Small STATIC TUs
# by design — the one justified exception to the header-only engine rule
# (platform syscalls cannot live in headers). Strict warnings like
# wengine_cli.
add_library(wengine_link_shm STATIC
    src/link/shm_publisher.cpp
    src/link/shm_reader.cpp
)
target_include_directories(wengine_link_shm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_link_shm PUBLIC wengine_core)
target_compile_options(wengine_link_shm PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)

# wengine-link socket endpoints (SIG-7): POSIX TCP/UDP publishers as
# emit::ISinks at the app edge. Small STATIC TU by the same justification
# as wengine_link_shm (platform syscalls cannot live in headers); headers
# stay platform-free (a Windows port replaces the TU, never the header).
add_library(wengine_link_net STATIC
    src/link/socket_publisher.cpp
    src/link/tcp_worker.cpp
    src/link/control_server.cpp
    src/link/control_client.cpp
    src/link/uart_publisher.cpp
)
target_include_directories(wengine_link_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_link_net PUBLIC wengine_core)
target_compile_options(wengine_link_net PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)

# ------------------------------------------------------------------
# ROS2 bridge (scaffolding + state-out publisher, D-ROS-1): edge-only, OFF
# by default so all builds stay ROS-free. Headers in
# include/wengine/ros2/* are ROS-free vocabulary (config/naming/verb
# mapping, topic helpers, transport-neutral publisher, header-only,
# covered by test_ros2_mapping + test_ros2_publisher without rclcpp).
# src/ros2/ros_publisher.cpp is the matching ROS-free TU (always built).
# The rclcpp TU (src/ros2/rclcpp_transport.cpp) + example below are the
# only places that may include ROS headers (same seam as src/link/*.cpp
# owning POSIX syscalls while link headers stay platform-free).
# ------------------------------------------------------------------
add_library(wengine_ros2 STATIC
    src/ros2/ros_publisher.cpp
)
target_include_directories(wengine_ros2 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_ros2 PUBLIC wengine_core)
target_compile_options(wengine_ros2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)
if(BUILD_ROS2)
    find_package(rclcpp REQUIRED)
    find_package(sensor_msgs REQUIRED)
    # wengine_msgs IDL package (introduced with ROS2-2 for the SigCommand
    # service; ROS2-3 extends it with the SignalBlock message). Built as a
    # subdirectory of src/ros2 so no ROS2 sources live in the top dir, and
    # only when the top-level BUILD_ROS2 option is ON (see above), so
    # default builds never require a ROS install.
    add_subdirectory(src/ros2/msgs)
    target_sources(wengine_ros2 PRIVATE src/ros2/rclcpp_transport.cpp src/ros2/ros_control.cpp)
    target_compile_definitions(wengine_ros2 PUBLIC WENGINE_HAS_RCLCPP=1)
    target_link_libraries(wengine_ros2 PUBLIC rclcpp::rclcpp sensor_msgs::sensor_msgs
        "${WENGINE_MSGS_CPP_TYPESUPPORT}")
    add_executable(ros2_bridge examples/ros2_bridge.cpp)
    target_link_libraries(ros2_bridge PRIVATE wengine_ros2)
    target_compile_options(ros2_bridge PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )
endif()

# ------------------------------------------------------------------
# Lua scripting source (SIG-4b): PUC Lua 5.4.9 (external/lua submodule,
# pinned v5.4.9, MIT) — the engine's third third-party dep (decision
# record: docs/signals.md sec. 7 + README dependency policy). Only the
# interpreter core + base/math/string libs are built (io/os/package/
# debug/coroutine/utf8 never compile — the sandbox registers selectively).
# ------------------------------------------------------------------
# Vendored interpreter, no project warning flags (third-party, like imgui).
set(WENGINE_LUA_SOURCES
    external/lua/lapi.c
    external/lua/lcode.c
    external/lua/lctype.c
    external/lua/ldebug.c
    external/lua/ldo.c
    external/lua/ldump.c
    external/lua/lfunc.c
    external/lua/lgc.c
    external/lua/llex.c
    external/lua/lmem.c
    external/lua/lobject.c
    external/lua/lopcodes.c
    external/lua/lparser.c
    external/lua/lstate.c
    external/lua/lstring.c
    external/lua/ltable.c
    external/lua/ltm.c
    external/lua/lundump.c
    external/lua/lvm.c
    external/lua/lzio.c
    external/lua/lauxlib.c
    external/lua/lbaselib.c
    external/lua/lmathlib.c
    external/lua/lstrlib.c
)
add_library(wengine_lua STATIC ${WENGINE_LUA_SOURCES})
# Lua compiles as C++ officially (luaconf.h handles __cplusplus, same as
# Ubuntu's liblua5.4-c++ builds); per-file language keeps the top-level
# LANGUAGES CXX untouched.
set_source_files_properties(${WENGINE_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(wengine_lua SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/lua>
    $<INSTALL_INTERFACE:include>
)

# Scripting-source runtime: the single TU allowed to know Lua exists.
# Engine headers stay Lua-free (see signal/lua.hpp); strict warnings like
# wengine_cli.
add_library(wengine_signal_lua STATIC
    src/signal/lua_solver.cpp
)
target_include_directories(wengine_signal_lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_signal_lua PUBLIC wengine_core wengine_lua)
target_compile_options(wengine_signal_lua PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)

# Logic workspace YAML (LOGIC-6): yaml-cpp (external/yaml-cpp submodule,
# pinned 0.8.0, MIT) — the engine's fourth third-party dep (decision
# record: docs/logic.md sec. 9 + README dependency policy). Parser +
# emitter core only (src/*.cpp); contrib graphbuilder never compiles.
# ------------------------------------------------------------------
# Vendored library, no project warning flags (third-party, like imgui/lua).
set(WENGINE_YAML_SOURCES
    external/yaml-cpp/src/binary.cpp
    external/yaml-cpp/src/convert.cpp
    external/yaml-cpp/src/depthguard.cpp
    external/yaml-cpp/src/directives.cpp
    external/yaml-cpp/src/emit.cpp
    external/yaml-cpp/src/emitfromevents.cpp
    external/yaml-cpp/src/emitter.cpp
    external/yaml-cpp/src/emitterstate.cpp
    external/yaml-cpp/src/emitterutils.cpp
    external/yaml-cpp/src/exceptions.cpp
    external/yaml-cpp/src/exp.cpp
    external/yaml-cpp/src/memory.cpp
    external/yaml-cpp/src/nodebuilder.cpp
    external/yaml-cpp/src/node.cpp
    external/yaml-cpp/src/node_data.cpp
    external/yaml-cpp/src/nodeevents.cpp
    external/yaml-cpp/src/null.cpp
    external/yaml-cpp/src/ostream_wrapper.cpp
    external/yaml-cpp/src/parse.cpp
    external/yaml-cpp/src/parser.cpp
    external/yaml-cpp/src/regex_yaml.cpp
    external/yaml-cpp/src/scanner.cpp
    external/yaml-cpp/src/scanscalar.cpp
    external/yaml-cpp/src/scantag.cpp
    external/yaml-cpp/src/scantoken.cpp
    external/yaml-cpp/src/simplekey.cpp
    external/yaml-cpp/src/singledocparser.cpp
    external/yaml-cpp/src/stream.cpp
    external/yaml-cpp/src/tag.cpp
)
add_library(wengine_yaml STATIC ${WENGINE_YAML_SOURCES})
target_include_directories(wengine_yaml SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/yaml-cpp/include>
    $<INSTALL_INTERFACE:include>
)
# Private scanner/parser headers live beside the sources (never public).
target_include_directories(wengine_yaml PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/yaml-cpp/src
)

# Versioned workspace store: the single TU allowed to know yaml-cpp exists.
# Engine headers stay yaml-free (see logic/yaml_store.hpp); strict warnings
# like wengine_cli.
add_library(wengine_logic_yaml STATIC
    src/logic/yaml_store.cpp
)
target_include_directories(wengine_logic_yaml PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(wengine_logic_yaml PUBLIC wengine_core wengine_yaml)
target_compile_options(wengine_logic_yaml PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
)

add_library(wengine_logic INTERFACE)
target_include_directories(wengine_logic INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(wengine_core INTERFACE)
target_link_libraries(wengine_core INTERFACE
    wengine_glm wengine_units wengine_signal wengine_solvers wengine_world wengine_emit wengine_link wengine_logic
)
add_library(wengine::core ALIAS wengine_core)

# ------------------------------------------------------------------
# CLI: thin adapter only (src/cli/*). No physics here, calls solvers.
# Future GUI (apps/gui) links wengine_core the same way, never the CLI.
# ------------------------------------------------------------------
if(BUILD_CLI)
    add_library(wengine_cli STATIC
        src/cli/shell.cpp
        src/cli/line_reader.cpp
        src/cli/complete.cpp
        src/cli/plot_sdl.cpp
        src/cli/logic_draw_sdl.cpp
        src/cli/commands/ohm.cpp
        src/cli/commands/s0.cpp
        src/cli/commands/i2c.cpp
        src/cli/commands/ldo.cpp
        src/cli/commands/led.cpp
        src/cli/commands/sim.cpp
        src/cli/commands/sig.cpp
        src/cli/commands/ic.cpp
        src/cli/commands/logic.cpp
        src/cli/commands/logic/add.cpp
        src/cli/commands/logic/common.cpp
        src/cli/commands/logic/draw.cpp
        src/cli/commands/logic/export.cpp
        src/cli/commands/logic/ignore.cpp
        src/cli/commands/logic/list.cpp
        src/cli/commands/logic/load.cpp
        src/cli/commands/logic/optimize.cpp
        src/cli/commands/logic/remove.cpp
        src/cli/commands/logic/reset.cpp
        src/cli/commands/logic/save.cpp
        src/cli/commands/logic/solve.cpp
        src/cli/commands/logic/truth.cpp
        src/cli/commands/logic/unwire.cpp
        src/cli/commands/logic/use.cpp
        src/cli/commands/logic/wire.cpp
        src/cli/commands/ic/ap63205.cpp
        src/cli/commands/ic/ina219.cpp
        src/cli/commands/ic/tmc22xx.cpp
    )
    target_include_directories(wengine_cli PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(wengine_cli PUBLIC wengine_core wengine_link_shm wengine_link_net wengine_signal_lua wengine_ros2 wengine_logic_yaml)

    target_compile_options(wengine_cli PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

    # Live `sig run --plot` window (system SDL2, like ncurses for the TUI:
    # never vendored). Optional: without SDL2 the CLI still builds and
    # --plot reports a rebuild hint at runtime (see wengine/cli/plot.hpp).
    find_package(SDL2 QUIET)
    if(TARGET SDL2::SDL2)
        target_link_libraries(wengine_cli PUBLIC SDL2::SDL2)
        target_compile_definitions(wengine_cli PRIVATE WENGINE_HAVE_SDL2=1)
        message(STATUS "WEngine live plot enabled (SDL2 --plot)")
    else()
        message(STATUS "WEngine live plot disabled (SDL2 not found: --plot reports a hint)")
    endif()

    # Interactive `logic draw` canvas: Dear ImGui over the SDL2 2D
    # renderer (GPU-AA vectors + TTF text + zoom/pan, no Vulkan needed —
    # imgui_impl_sdlrenderer2 needs SDL2 only). Vendored imgui, SYSTEM
    # includes, no project warning flags — same treatment as
    # wengine_imgui. Only when SDL2 + the imgui submodule are both
    # present; otherwise `draw` reports a rebuild hint (the BMP export
    # keeps its SDL-only software path either way).
    if(TARGET SDL2::SDL2 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/imgui.h")
        add_library(wengine_imgui_sdl STATIC
            external/imgui/imgui.cpp
            external/imgui/imgui_draw.cpp
            external/imgui/imgui_tables.cpp
            external/imgui/imgui_widgets.cpp
            external/imgui/backends/imgui_impl_sdl2.cpp
            external/imgui/backends/imgui_impl_sdlrenderer2.cpp
        )
        target_include_directories(wengine_imgui_sdl SYSTEM PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/imgui>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends>
        )
        target_link_libraries(wengine_imgui_sdl PUBLIC SDL2::SDL2)
        target_link_libraries(wengine_cli PRIVATE wengine_imgui_sdl)
        target_compile_definitions(wengine_cli PRIVATE WENGINE_HAVE_IMGUI_DRAW=1)
        target_sources(wengine_cli PRIVATE src/cli/logic_draw_imgui.cpp)
        message(STATUS "WEngine schematic canvas enabled (ImGui draw)")
    else()
        message(STATUS "WEngine schematic canvas disabled (needs SDL2 + imgui submodule: draw reports a hint)")
    endif()

    add_executable(PhysicsSimApp apps/cli/main.cpp)
    target_link_libraries(PhysicsSimApp PRIVATE wengine_cli)

    # Sample external-app reader for the wengine-link SHM bus; pairs with
    # `sim run --emit-shm`. Small, self-contained, strict warnings.
    add_executable(link_sample examples/link_sample.cpp)
    target_link_libraries(link_sample PRIVATE wengine_core wengine_link_shm)
    target_compile_options(link_sample PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

    # Replay bridge between CsvSink logs and the bus (play/record modes).
    add_executable(link_bridge examples/link_bridge.cpp)
    target_link_libraries(link_bridge PRIVATE wengine_core wengine_link_shm)
    target_compile_options(link_bridge PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

    # Minimum-loss subscriber: no per-frame printing, reused decode buffers.
    add_executable(link_fastread examples/link_fastread.cpp)
    target_link_libraries(link_fastread PRIVATE wengine_core wengine_link_shm)
    target_compile_options(link_fastread PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

    # TUI (system libncurses, like SDL2/Vulkan for the GUI: never vendored).
    # `wengine_tui` owns terminal handling only; command execution reuses
    # `wengine_cli` verbatim via `wengine/tui/exec.hpp`, so TUI output stays
    # byte-identical to the CLI by construction. Graceful fallback: without
    # Curses the CLI still builds and `--tui` prints a rebuild hint.
    if(BUILD_TUI)
        set(CURSES_NEED_NCURSES TRUE)
        find_package(Curses)
        if(CURSES_FOUND)
            add_library(wengine_tui STATIC
                src/tui/run.cpp
                src/tui/state.cpp
                src/tui/text.cpp
                src/tui/submit.cpp
                src/tui/edit.cpp
                src/tui/draw_main.cpp
                src/tui/panes_help.cpp
                src/tui/panes_forms.cpp
                src/tui/panes_batches.cpp
                src/tui/panes_settings.cpp
                src/tui/keys.cpp
            )
            target_include_directories(wengine_tui PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            )
            target_link_libraries(wengine_tui PUBLIC wengine_cli)
            if(TARGET Curses::Curses)
                target_link_libraries(wengine_tui PRIVATE Curses::Curses)
            else()
                target_include_directories(wengine_tui PRIVATE ${CURSES_INCLUDE_DIR})
                target_link_libraries(wengine_tui PRIVATE ${CURSES_LIBRARIES})
            endif()
            target_compile_options(wengine_tui PRIVATE
                $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
                $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
            )
            target_link_libraries(PhysicsSimApp PRIVATE wengine_tui)
            target_compile_definitions(PhysicsSimApp PRIVATE WENGINE_HAVE_TUI=1)
            message(STATUS "WEngine TUI enabled (ncurses --tui)")
        else()
            message(WARNING "Curses not found: PhysicsSimApp builds without --tui support")
        endif()
    endif()
endif()

# ------------------------------------------------------------------
# GUI (PHYSI-13 decision: Dear ImGui + SDL2 + Vulkan).
# - Dear ImGui: external/imgui submodule, pinned v1.91.9b (MIT), the
#   engine's second third-party dep (see README dependency policy).
# - SDL2 + Vulkan loader: system packages (libsdl2-dev, libvulkan-dev).
# apps/gui links wengine_core + wengine_link_shm (SHM subscribe endpoints)
# directly — never wengine_cli, never the reverse — and owns all I/O.
# No engine headers change for GUI work.
# ------------------------------------------------------------------
if(BUILD_GUI)
    find_package(SDL2 CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)

    # Third-party: deliberately no project warning flags here.
    add_library(wengine_imgui STATIC
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_sdl2.cpp
        external/imgui/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(wengine_imgui SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/imgui>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends>
    )
    target_link_libraries(wengine_imgui PUBLIC SDL2::SDL2 Vulkan::Vulkan)

    add_executable(WEngineGui
        apps/gui/main.cpp
        apps/gui/backend/vk_context.cpp
        apps/gui/panels/closed_form_panels.cpp
    )
    target_include_directories(WEngineGui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    target_link_libraries(WEngineGui PRIVATE wengine_core wengine_imgui wengine_link_shm
        wengine_signal_lua)
    target_compile_options(WEngineGui PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )
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

    # TUI executor parity: links wengine_cli only (no terminal needed).
    add_executable(test_tui_exec tests/test_tui_exec.cpp)
    target_link_libraries(test_tui_exec PRIVATE wengine_cli)
    add_test(NAME tui_exec COMMAND test_tui_exec)

    # TUI closed-form guided forms: compose-only parity matrix.
    add_executable(test_tui_forms tests/test_tui_forms.cpp)
    target_link_libraries(test_tui_forms PRIVATE wengine_cli)
    add_test(NAME tui_forms COMMAND test_tui_forms)

    # TUI sim/sig run builders: compose-only parity matrix + async routing.
    add_executable(test_tui_batches tests/test_tui_batches.cpp)
    target_link_libraries(test_tui_batches PRIVATE wengine_cli)
    add_test(NAME tui_batches COMMAND test_tui_batches)

    # TUI enum-choice dialogs: static + registry-derived options + model.
    add_executable(test_tui_choice tests/test_tui_choice.cpp)
    target_link_libraries(test_tui_choice PRIVATE wengine_cli)
    add_test(NAME tui_choice COMMAND test_tui_choice)

    # TUI async worker: parity + busy/cancel/progress/emit-csv (needs threads).
    add_executable(test_tui_async tests/test_tui_async.cpp)
    target_link_libraries(test_tui_async PRIVATE wengine_cli)
    find_package(Threads REQUIRED)
    target_link_libraries(test_tui_async PRIVATE Threads::Threads)
    add_test(NAME tui_async COMMAND test_tui_async)

    # TUI-6 output views: highlight/search/save/resize/no-color (ncurses-free).
    add_executable(test_tui_views tests/test_tui_views.cpp)
    target_link_libraries(test_tui_views PRIVATE wengine_cli)
    add_test(NAME tui_views COMMAND test_tui_views)

    # TUI-7 PTY smoke: launch/run/complete/recall/quit headlessly in CI.
    # Real PTY via python3 pty.fork (no display needed); isolated history
    # via a temp XDG_CACHE_HOME. Needs a cursor-addressable $TERM at run
    # time (the script sets xterm itself) plus the built PhysicsSimApp.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME tui_pty_smoke
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui_pty_smoke.py
                    --bin $<TARGET_FILE:PhysicsSimApp>)
        set_tests_properties(tui_pty_smoke PROPERTIES TIMEOUT 120)
        # LOGIC-1 session proof: prompt swap + scoped Tab + back, on a real
        # PTY (raw-mode editing never engages on pipes).
        add_test(NAME logic_pty_smoke
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/logic_pty_smoke.py
                    --bin $<TARGET_FILE:PhysicsSimApp>)
        set_tests_properties(logic_pty_smoke PROPERTIES TIMEOUT 120)
    endif()

    add_executable(test_s0 tests/test_s0.cpp)
    target_link_libraries(test_s0 PRIVATE wengine_cli)
    add_test(NAME s0 COMMAND test_s0)

    add_executable(test_i2c tests/test_i2c.cpp)
    target_link_libraries(test_i2c PRIVATE wengine_cli)
    add_test(NAME i2c COMMAND test_i2c)

    add_executable(test_ldo tests/test_ldo.cpp)
    target_link_libraries(test_ldo PRIVATE wengine_cli)
    add_test(NAME ldo COMMAND test_ldo)

    add_executable(test_led tests/test_led.cpp)
    target_link_libraries(test_led PRIVATE wengine_cli)
    add_test(NAME led COMMAND test_led)

    add_executable(test_ic tests/test_ic.cpp)
    target_link_libraries(test_ic PRIVATE wengine_cli)
    add_test(NAME ic COMMAND test_ic)

    add_executable(test_ap63205 tests/test_ap63205.cpp)
    target_link_libraries(test_ap63205 PRIVATE wengine_cli)
    add_test(NAME ap63205 COMMAND test_ap63205)

    add_executable(test_ina219 tests/test_ina219.cpp)
    target_link_libraries(test_ina219 PRIVATE wengine_cli)
    add_test(NAME ina219 COMMAND test_ina219)

    add_executable(test_tmc22xx tests/test_tmc22xx.cpp)
    target_link_libraries(test_tmc22xx PRIVATE wengine_cli)
    add_test(NAME tmc22xx COMMAND test_tmc22xx)

    add_executable(test_sim tests/test_sim.cpp)
    target_link_libraries(test_sim PRIVATE wengine_cli)
    add_test(NAME sim COMMAND test_sim)
    if(BUILD_ROS2)
        # Live `sig run --emit-ros` in test_sim dlopens the rmw typesupport
        # by soname (standard rosidl dispatch): prepend the fresh build-tree
        # msgs lib dir so the just-built SignalBlock/SigCommand support is
        # found without a sourced install (sourcing ROS itself stays the
        # caller's job, like every other ROS run).
        set_tests_properties(sim PROPERTIES ENVIRONMENT_MODIFICATION
            "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/src/ros2/msgs")
    endif()

    add_executable(test_solver_registry tests/test_solver_registry.cpp)
    target_link_libraries(test_solver_registry PRIVATE wengine_core)
    add_test(NAME solver_registry COMMAND test_solver_registry)

    add_executable(test_signal tests/test_signal.cpp)
    target_link_libraries(test_signal PRIVATE wengine_core)
    add_test(NAME signal COMMAND test_signal)

    add_executable(test_logic tests/test_logic.cpp)
    target_link_libraries(test_logic PRIVATE wengine_core)
    add_test(NAME logic COMMAND test_logic)

    # LOGIC-6 workspace store: yaml-cpp seam TU + yaml-free header. Links
    # wengine_logic_yaml only (never wengine_cli); test_logic above stays
    # on wengine_core alone by design.
    add_executable(test_logic_yaml tests/test_logic_yaml.cpp)
    target_link_libraries(test_logic_yaml PRIVATE wengine_logic_yaml)
    # Fixture path: absolute source directory so out-of-tree builds
    # (-B /tmp/..., including paths with spaces) load the shipped
    # logic examples instead of assuming build/ sits inside the repo.
    target_compile_definitions(test_logic_yaml PRIVATE
        "WENGINE_LOGIC_EXAMPLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/examples/logic\"")
    add_test(NAME logic_yaml COMMAND test_logic_yaml)

    # LOGIC-7 schematic layout: pure header-only geometry (no SDL), links
    # wengine_core alone like test_logic.
    add_executable(test_logic_layout tests/test_logic_layout.cpp)
    target_link_libraries(test_logic_layout PRIVATE wengine_core)
    add_test(NAME logic_layout COMMAND test_logic_layout)

    # ROS2 scaffolding: pure mapping/naming/verb vocabulary, no rclcpp.
    add_executable(test_ros2_mapping tests/test_ros2_mapping.cpp)
    target_link_libraries(test_ros2_mapping PRIVATE wengine_core)
    add_test(NAME ros2_mapping COMMAND test_ros2_mapping)

    # ROS2 state-out publisher: transport-neutral ISink half, fake
    # transport, no rclcpp (the rmw TU is covered by live HIL instead).
    add_executable(test_ros2_publisher tests/test_ros2_publisher.cpp)
    target_link_libraries(test_ros2_publisher PRIVATE wengine_ros2)
    add_test(NAME ros2_publisher COMMAND test_ros2_publisher)

    # ROS2 control plane: service vocabulary + step-boundary drain, no
    # rclcpp (the rmw services are covered by live HIL instead).
    add_executable(test_ros2_control tests/test_ros2_control.cpp)
    target_link_libraries(test_ros2_control PRIVATE wengine_core)
    add_test(NAME ros2_control COMMAND test_ros2_control)

    # ROS2-4 QoS/clock/delivery policy + lifecycle: pinned matrix, stamp
    # sync, observable counters, transition graph — no rclcpp.
    add_executable(test_ros2_lifecycle tests/test_ros2_lifecycle.cpp)
    target_link_libraries(test_ros2_lifecycle PRIVATE wengine_core wengine_ros2)
    add_test(NAME ros2_lifecycle COMMAND test_ros2_lifecycle)

    # Lua scripting source: isolated runtime + sandbox pin tests.
    add_executable(test_lua tests/test_lua.cpp)
    target_link_libraries(test_lua PRIVATE wengine_signal_lua)
    # Fixture path: absolute source directory so out-of-tree builds
    # (-B /tmp/..., including paths with spaces) load the shipped
    # examples instead of assuming build/ sits inside the repo.
    target_compile_definitions(test_lua PRIVATE
        "WENGINE_LUA_EXAMPLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/examples/lua\"")
    add_test(NAME lua COMMAND test_lua)

    add_executable(test_routing tests/test_routing.cpp)
    target_link_libraries(test_routing PRIVATE wengine_core)
    add_test(NAME routing COMMAND test_routing)

    # SIG-10 generic control plane: engine verbs/bus/replay + TCP server.
    add_executable(test_signal_control tests/test_signal_control.cpp)
    target_link_libraries(test_signal_control PRIVATE wengine_core)
    add_test(NAME signal_control COMMAND test_signal_control)

    add_executable(test_control_server tests/test_control_server.cpp)
    target_link_libraries(test_control_server PRIVATE wengine_cli)
    add_test(NAME control_server COMMAND test_control_server)

    # SIG-8 UART HIL track: wire codec (+ shared Zephyr C parsers, built
    # here as C++ for the host cross-check) and pty-loopback publisher.
    add_executable(test_uart_codec tests/test_uart_codec.cpp
        examples/zephyr/common/wuart.c
        examples/zephyr/common/wcompact.c
    )
    target_link_libraries(test_uart_codec PRIVATE wengine_core)
    target_include_directories(test_uart_codec PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/zephyr/common
    )
    set_source_files_properties(examples/zephyr/common/wuart.c
        examples/zephyr/common/wcompact.c PROPERTIES LANGUAGE CXX)
    add_test(NAME uart_codec COMMAND test_uart_codec)

    add_executable(test_uart_pub tests/test_uart_pub.cpp)
    target_link_libraries(test_uart_pub PRIVATE wengine_cli)
    add_test(NAME uart_pub COMMAND test_uart_pub)

    add_executable(test_runner tests/test_runner.cpp)
    target_link_libraries(test_runner PRIVATE wengine_core)
    add_test(NAME runner COMMAND test_runner)

    add_executable(test_world tests/test_world.cpp)
    target_link_libraries(test_world PRIVATE wengine_core)
    add_test(NAME world COMMAND test_world)

    add_executable(test_emit tests/test_emit.cpp)
    target_link_libraries(test_emit PRIVATE wengine_core)
    add_test(NAME emit COMMAND test_emit)

    add_executable(test_snapshot_reuse tests/test_snapshot_reuse.cpp)
    target_link_libraries(test_snapshot_reuse PRIVATE wengine_core)
    add_test(NAME snapshot_reuse COMMAND test_snapshot_reuse)

    add_executable(test_particle_identity tests/test_particle_identity.cpp)
    target_link_libraries(test_particle_identity PRIVATE wengine_core)
    add_test(NAME particle_identity COMMAND test_particle_identity)

    add_executable(test_link_protocol tests/test_link_protocol.cpp)
    target_link_libraries(test_link_protocol PRIVATE wengine_core)
    add_test(NAME link_protocol COMMAND test_link_protocol)

    add_executable(test_link_shm tests/test_link_shm.cpp)
    target_link_libraries(test_link_shm PRIVATE wengine_core wengine_link_shm)
    add_test(NAME link_shm COMMAND test_link_shm)

    add_executable(test_link_sockets tests/test_link_sockets.cpp)
    target_link_libraries(test_link_sockets PRIVATE wengine_core wengine_link_net)
    add_test(NAME link_sockets COMMAND test_link_sockets)

    add_executable(test_tcp_worker tests/test_tcp_worker.cpp)
    target_link_libraries(test_tcp_worker PRIVATE wengine_core wengine_link_net)
    add_test(NAME tcp_worker COMMAND test_tcp_worker)

    add_executable(test_output_health tests/test_output_health.cpp)
    target_link_libraries(test_output_health PRIVATE wengine_core wengine_link_shm wengine_link_net)
    add_test(NAME output_health COMMAND test_output_health)

    add_executable(test_link_compact tests/test_link_compact.cpp)
    target_link_libraries(test_link_compact PRIVATE wengine_core)
    add_test(NAME link_compact COMMAND test_link_compact)

    add_executable(test_link_sample_block tests/test_link_sample_block.cpp)
    target_link_libraries(test_link_sample_block PRIVATE wengine_core)
    add_test(NAME link_sample_block COMMAND test_link_sample_block)

    add_executable(test_output_queue tests/test_output_queue.cpp)
    target_link_libraries(test_output_queue PRIVATE wengine_core)
    add_test(NAME output_queue COMMAND test_output_queue)

    add_executable(test_link_receiver tests/test_link_receiver.cpp)
    target_link_libraries(test_link_receiver PRIVATE wengine_core)
    add_test(NAME link_receiver COMMAND test_link_receiver)
    add_executable(test_link_bridge tests/test_link_bridge.cpp)
    target_link_libraries(test_link_bridge PRIVATE wengine_core wengine_link_shm)
    add_test(NAME link_bridge COMMAND test_link_bridge)

    # GUI external sources: engine-only controller header, no SDL/Vulkan needed.
    add_executable(test_gui_stream_source tests/test_gui_stream_source.cpp)
    target_link_libraries(test_gui_stream_source PRIVATE wengine_core wengine_link_shm)
    target_include_directories(test_gui_stream_source PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME gui_stream_source COMMAND test_gui_stream_source)

    add_executable(test_run_config tests/test_run_config.cpp)
    target_link_libraries(test_run_config PRIVATE wengine_core)
    add_test(NAME run_config COMMAND test_run_config)

    add_executable(test_paced tests/test_paced.cpp)
    target_link_libraries(test_paced PRIVATE wengine_cli)
    add_test(NAME paced COMMAND test_paced)

    add_executable(test_determinism tests/test_determinism.cpp)
    target_link_libraries(test_determinism PRIVATE wengine_core)
    add_test(NAME determinism COMMAND test_determinism)

    # GUI controller test: engine-only header, no SDL/Vulkan needed.
    add_executable(test_gui_sim_control tests/test_gui_sim_control.cpp)
    target_link_libraries(test_gui_sim_control PRIVATE wengine_core)
    target_include_directories(test_gui_sim_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME gui_sim_control COMMAND test_gui_sim_control)

    # GUI signal panel model + controller parity: engine-only headers.
    add_executable(test_signal_panel tests/test_signal_panel.cpp)
    target_link_libraries(test_signal_panel PRIVATE wengine_core)
    target_include_directories(test_signal_panel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME signal_panel COMMAND test_signal_panel)

    # GUI controller scripting-runtime wiring: needs the Lua extra solver.
    add_executable(test_signal_panel_lua tests/test_signal_panel_lua.cpp)
    target_link_libraries(test_signal_panel_lua PRIVATE wengine_core wengine_signal_lua)
    target_include_directories(test_signal_panel_lua PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME signal_panel_lua COMMAND test_signal_panel_lua)

    # Closed-form panel models: engine-only header (+ registry declaration),
    # no ImGui link needed.
    add_executable(test_closed_form_panels tests/test_closed_form_panels.cpp)
    target_link_libraries(test_closed_form_panels PRIVATE wengine_core)
    target_include_directories(test_closed_form_panels PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME closed_form_panels COMMAND test_closed_form_panels)

    # 3D viewport math: glm-double header, no ImGui/SDL/Vulkan needed.
    add_executable(test_viewport3d tests/test_viewport3d.cpp)
    target_link_libraries(test_viewport3d PRIVATE wengine_core)
    target_include_directories(test_viewport3d PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME viewport3d COMMAND test_viewport3d)

    # Frame source seam: adapter + history, engine-only header.
    # (The plot bounds-math check includes the graphs panel + imgui
    # headers only; nothing ImGui is linked, like glm via wengine_core.)
    add_executable(test_frame_source tests/test_frame_source.cpp)
    target_link_libraries(test_frame_source PRIVATE wengine_core)
    target_include_directories(test_frame_source PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    )
    add_test(NAME frame_source COMMAND test_frame_source)

    # 2D world-view math: doubles-only header, no ImGui/SDL/Vulkan needed.
    add_executable(test_canvas2d tests/test_canvas2d.cpp)
    target_link_libraries(test_canvas2d PRIVATE wengine_core)
    target_include_directories(test_canvas2d PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/gui
    )
    add_test(NAME canvas2d COMMAND test_canvas2d)

    # Assertion policy pin: fails the build if NDEBUG is ever defined
    # for a test TU, and fails at runtime if an assert side effect is
    # ever skipped. New test_* targets are picked up automatically by
    # the loop below (keep the test_ prefix).
    add_executable(test_assert_policy tests/test_assert_policy.cpp)
    target_link_libraries(test_assert_policy PRIVATE wengine_core)
    add_test(NAME assert_policy COMMAND test_assert_policy)

    # Intentional-failure probe: assert(false) must abort even in an
    # optimized build. CTest WILL_FAIL cannot invert a crash (an abort
    # is reported as ***Exception failure regardless), so a small
    # harness inverts the status: probe aborts -> pass, probe exits
    # 0 -> assertions disabled -> fail loudly instead of silently
    # passing.
    add_executable(assert_fail_probe tests/assert_fail_probe.cpp)
    target_link_libraries(assert_fail_probe PRIVATE wengine_core)
    add_test(NAME assert_probe
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_probe_check.sh
            $<TARGET_FILE:assert_fail_probe>)

    # Apply the assertion policy to every test executable defined
    # above. Deliberately scoped here (not via link_libraries): bench_*
    # targets below must keep their own NDEBUG/optimization behavior.
    get_property(_wengine_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _wengine_test_targets)
        if(_t MATCHES "^(test_|assert_fail_probe$)")
            if(TARGET ${_t})
                get_target_property(_ttype ${_t} TYPE)
                if(_ttype STREQUAL "EXECUTABLE")
                    target_link_libraries(${_t} PRIVATE wengine_test_assert)
                endif()
            endif()
        endif()
    endforeach()
    unset(_wengine_test_targets)
    unset(_t)
    unset(_ttype)
endif()

if(BUILD_BENCHMARKS)
    # Benchmarks keep the build-type NDEBUG/optimization untouched and
    # never link wengine_test_assert (see above). Effective flags are
    # reported at configure time and pinned by the bench preset in
    # CMakePresets.json; verify with build-bench/CMakeCache.txt
    # (CMAKE_CXX_FLAGS_RELEASE) or compile_commands.json.
    message(STATUS "WEngine benchmarks: BUILD_TYPE='${CMAKE_BUILD_TYPE}' "
        "CXX=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "FLAGS='${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}'")
    add_executable(bench_ohm benchmarks/bench_ohm.cpp)
    target_link_libraries(bench_ohm PRIVATE wengine_core)

    add_executable(bench_s0 benchmarks/bench_s0.cpp)
    target_link_libraries(bench_s0 PRIVATE wengine_core)

    add_executable(bench_i2c benchmarks/bench_i2c.cpp)
    target_link_libraries(bench_i2c PRIVATE wengine_core)

    add_executable(bench_projectile benchmarks/bench_projectile.cpp)
    target_link_libraries(bench_projectile PRIVATE wengine_core)

    add_executable(bench_signal benchmarks/bench_signal.cpp)
    target_link_libraries(bench_signal PRIVATE wengine_core)

    # Sparse-world / serial-lookup / custom-source scaling (pool-layout
    # baseline before any parallelism or layout change). Links the isolated
    # Lua runtime like test_lua to separate Lua step costs.
    add_executable(bench_signal_scale benchmarks/bench_signal_scale.cpp)
    target_link_libraries(bench_signal_scale PRIVATE wengine_core wengine_signal_lua)

    add_executable(bench_snapshot benchmarks/bench_snapshot.cpp)
    target_link_libraries(bench_snapshot PRIVATE wengine_core)

    add_executable(bench_link benchmarks/bench_link.cpp)
    target_link_libraries(bench_link PRIVATE wengine_core wengine_link_shm)

    add_executable(bench_link_sockets benchmarks/bench_link_sockets.cpp)
    target_link_libraries(bench_link_sockets PRIVATE wengine_core wengine_link_net)

    add_executable(bench_sample_block benchmarks/bench_sample_block.cpp)
    target_link_libraries(bench_sample_block PRIVATE wengine_core)

    add_executable(bench_tcp_worker benchmarks/bench_tcp_worker.cpp)
    target_link_libraries(bench_tcp_worker PRIVATE wengine_core wengine_link_net)

    add_executable(bench_logic benchmarks/bench_logic.cpp)
    target_link_libraries(bench_logic PRIVATE wengine_core)
endif()
