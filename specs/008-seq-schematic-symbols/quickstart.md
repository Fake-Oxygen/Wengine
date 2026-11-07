# Quickstart: Validating Sequential Schematic Symbols

**Feature**: `008-seq-schematic-symbols` | **Date**: 2026-11-07
Validates the feature end-to-end. Prerequisites: configured build
(`cmake -S . -B build -DBUILD_TESTS=ON`), SDL2 available for the BMP
path; a display + imgui build only for step 4 (manual).

## 1. Layout goldens (headless, engine only)

```sh
cmake --build build --target test_logic_layout
./build/test_logic_layout
# expect: test_logic_layout: OK
```

Covers: DFF bare + full-control anchors share D/CLK rows (contract
C2, invariant I-CLK), JK/T/DLATCH/SRLATCH rows, shared-clock routes
landing horizontal at row 2/6, NQ-source route starting at the NQ
anchor, dangling-clock flop (layout succeeds, no wire), repeat-layout
identity — plus all pre-existing combinational goldens unchanged.

## 2. BMP export path (headless, needs SDL2 build)

In the app REPL / one-shot CLI (exact verb spelling per `docs/logic.md`
§10; sketch — adjust to the shipped `logic` scoping):

```text
logic add DFF --inputs 2        # DFF1
logic add DFF --inputs 5        # DFF2 (full control set)
logic add AND                   # glue
logic wire AND1.Y DFF1.D
logic wire AND1.Y DFF2.CLK      # shared driver onto data + clock
logic draw --out /tmp/opencode/seq_schem.bmp
# expect: drew 3 gates (N levels) (WxH) with W,H == 2x layout canvas
```

Then confirm: output is a `BM`-magic BMP with pixel dims exactly twice
the `layout_schematic` canvas, and dark ink exists inside the former
empty-box regions (wedge + rectangle + face letters). The automated
form of these assertions lives in `tests/test_cli_args.cpp`; run:

```sh
cmake --build build --target test_cli_args
./build/test_cli_args
# expect: test_cli_args: OK
```

## 3. Full suite (regression guard)

```sh
ctest --test-dir build --output-on-failure
# expect: all green — in particular combinational draw/export behavior
# (SC-004) and every sequential stepping/optimizer suite untouched.
```

## 4. Interactive canvas (manual, display build only)

```sh
cmake -S . -B build-gui -DBUILD_GUI=ON -DBUILD_TESTS=OFF  # if needed
# or: SDL2 + imgui CLI build, then:
./build/PhysicsSimApp   # → logic session → build the §2 net → logic draw
# expect: window shows rectangles with D faces, wedge triangles on both
# CLK anchors at equal height, NQ bubble where wired, notch on any latch;
# wheel-zoom / F-fit / R-1:1 / Esc behave as before. Compare against the
# BMP from step 2: same boxes, anchors, corners (spec SC-003).
```

Without a display the window verb must fail loudly with the rebuild
hint (unchanged contract) while `--out` still works.

## Links

- Normative geometry: [contracts/layout-anchors.md](contracts/layout-anchors.md)
- Entities and derivations: [data-model.md](data-model.md)
- Decisions and rejected alternatives: [research.md](research.md)
- User-visible behavior: [spec.md](spec.md); canvas docs: `docs/logic.md` §8
