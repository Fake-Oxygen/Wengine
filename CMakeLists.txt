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
option(BUILD_TESTS "Build lightweight tests (no external deps)" ON)
option(BUILD_BENCHMARKS "Build micro-benchmarks" ON)

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

add_library(wengine_core INTERFACE)
target_link_libraries(wengine_core INTERFACE
    wengine_glm wengine_units wengine_signal wengine_solvers wengine_world wengine_emit wengine_link
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
        src/cli/commands/ohm.cpp
        src/cli/commands/s0.cpp
        src/cli/commands/i2c.cpp
        src/cli/commands/sim.cpp
        src/cli/commands/sig.cpp
        src/cli/commands/ic.cpp
    )
    target_include_directories(wengine_cli PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(wengine_cli PUBLIC wengine_core wengine_link_shm wengine_link_net wengine_signal_lua)

    target_compile_options(wengine_cli PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor>
    )

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

    add_executable(test_s0 tests/test_s0.cpp)
    target_link_libraries(test_s0 PRIVATE wengine_cli)
    add_test(NAME s0 COMMAND test_s0)

    add_executable(test_i2c tests/test_i2c.cpp)
    target_link_libraries(test_i2c PRIVATE wengine_cli)
    add_test(NAME i2c COMMAND test_i2c)

    add_executable(test_ic tests/test_ic.cpp)
    target_link_libraries(test_ic PRIVATE wengine_cli)
    add_test(NAME ic COMMAND test_ic)

    add_executable(test_sim tests/test_sim.cpp)
    target_link_libraries(test_sim PRIVATE wengine_cli)
    add_test(NAME sim COMMAND test_sim)

    add_executable(test_solver_registry tests/test_solver_registry.cpp)
    target_link_libraries(test_solver_registry PRIVATE wengine_core)
    add_test(NAME solver_registry COMMAND test_solver_registry)

    add_executable(test_signal tests/test_signal.cpp)
    target_link_libraries(test_signal PRIVATE wengine_core)
    add_test(NAME signal COMMAND test_signal)

    # Lua scripting source: isolated runtime + sandbox pin tests.
    add_executable(test_lua tests/test_lua.cpp)
    target_link_libraries(test_lua PRIVATE wengine_signal_lua)
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
endif()

if(BUILD_BENCHMARKS)
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

    add_executable(bench_link benchmarks/bench_link.cpp)
    target_link_libraries(bench_link PRIVATE wengine_core wengine_link_shm)

    add_executable(bench_link_sockets benchmarks/bench_link_sockets.cpp)
    target_link_libraries(bench_link_sockets PRIVATE wengine_core wengine_link_net)

    add_executable(bench_sample_block benchmarks/bench_sample_block.cpp)
    target_link_libraries(bench_sample_block PRIVATE wengine_core)

    add_executable(bench_tcp_worker benchmarks/bench_tcp_worker.cpp)
    target_link_libraries(bench_tcp_worker PRIVATE wengine_core wengine_link_net)
endif()
