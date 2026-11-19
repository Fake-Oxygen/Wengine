#!/usr/bin/env python3
"""Catalog completeness audit for the WEngine documentation suite.

Cross-checks the documentation catalogs (``docs/tests/``, ``docs/benchmarks/``,
``docs/examples/``) against the live artifact inventories (contracts/
catalog-schemas.md CS5). This is the SC-001/SC-004 gate: "auditable by
construction" means running this script at delivery reports zero gaps.

Inventory sources (what MUST be documented):
  * tests      -- every ``add_test(...)`` registration in CMakeLists.txt,
                  plus every test-support file in tests/ (``*.sh``,
                  ``*.py``, ``*.hpp``).
  * benchmarks -- every file in benchmarks/.
  * examples   -- every file in examples/ (recursive; the Zephyr embedded
                  sample is one unit, ``zephyr/``, covering its whole tree).

Catalog sources (what IS documented):
  * level-2 headings in docs/tests/*.md, docs/benchmarks/*.md,
    docs/examples/*.md following the anchor schemas:
      - test targets:        ``## test_<target>``
      - test-support files:  ``## <file-name>`` (probe, scripts, fixtures)
      - benchmarks:          ``## bench_<name>``
      - examples:            ``## <path relative to examples/>``
        (``zephyr/`` for the embedded sample as a whole)

Behavior:
  * exit 0  -- prints ``N/N covered`` per category.
  * exit 1  -- prints every inventory item with no catalog anchor (missing)
               and every catalog anchor with no inventory item (ghost;
               renamed artifacts must not leave stale entries).

Stdlib only; no third-party imports.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = REPO_ROOT / "CMakeLists.txt"
TESTS_DIR = REPO_ROOT / "tests"
BENCHMARKS_DIR = REPO_ROOT / "benchmarks"
EXAMPLES_DIR = REPO_ROOT / "examples"
DOCS_TESTS = Path(__file__).resolve().parent / "tests"
DOCS_BENCHMARKS = Path(__file__).resolve().parent / "benchmarks"
DOCS_EXAMPLES = Path(__file__).resolve().parent / "examples"

# Files/directories never treated as example artifacts.
EXAMPLE_EXCLUDE_DIRS = {"__pycache__"}


def fail(message: str) -> None:
    print(f"audit_catalogs.py: {message}", file=sys.stderr)


# --------------------------------------------------------------------------
# Inventories
# --------------------------------------------------------------------------

def slice_add_tests(cmake_text: str) -> list[str]:
    """Return each ``add_test(...)`` argument region (multi-line safe).

    add_test calls here never nest parentheses except inside generator
    expressions (``$<...>``) and variable references (``${...}``), which do
    not contain raw parens, so scanning to the next ``)`` is exact.
    """
    slices: list[str] = []
    for match in re.finditer(r"add_test\s*\(", cmake_text):
        start = match.end()
        depth = 1
        pos = start
        while pos < len(cmake_text) and depth:
            char = cmake_text[pos]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            pos += 1
        slices.append(cmake_text[start : pos - 1])
    return slices


def test_inventory() -> dict[str, str]:
    """Inventory of test artifacts: {anchor: description}.

    Anchors come from the add_test COMMAND tokens:
      * plain ``test_*`` targets   -> ``test_<target>``
      * ``$<TARGET_FILE:X>`` where X is a probe/test binary -> ``X``
      * referenced ``*.py``/``*.sh`` scripts -> file name
    plus every support file in tests/ (``*.sh``, ``*.py``, ``*.hpp``).
    """
    units: dict[str, str] = {}

    cmake_text = CMAKE_FILE.read_text(encoding="utf-8")
    for region in slice_add_tests(cmake_text):
        # Plain test targets referenced as COMMAND tokens.
        for token in re.findall(r"(?<![\w./$>-])test_\w+", region):
            units.setdefault(token, "CTest-registered test binary")
        # Generator-expression targets (probe binaries).
        for target in re.findall(r"\$<TARGET_FILE:([\w-]+)>", region):
            if target.startswith("test_") or target.startswith("assert_"):
                units.setdefault(target, "CTest-invoked probe binary")
        # Script harnesses invoked by add_test.
        for script in re.findall(r"[\w.-]+\.(?:py|sh)\b", region):
            units.setdefault(script, "CTest-invoked support script")

    # Support artifacts living in tests/ that no add_test names directly
    # (fixture headers, checker scripts).
    if TESTS_DIR.is_dir():
        for path in sorted(TESTS_DIR.iterdir()):
            if path.suffix in {".sh", ".py", ".hpp"}:
                units.setdefault(path.name, f"test-support file ({path.name})")

    return units


def benchmark_inventory() -> dict[str, str]:
    """Inventory of benchmarks: {anchor: description}.

    Anchor is the source stem (``bench_ohm`` for ``bench_ohm.cpp``).
    """
    units: dict[str, str] = {}
    if BENCHMARKS_DIR.is_dir():
        for path in sorted(BENCHMARKS_DIR.iterdir()):
            if path.suffix == ".cpp":
                units.setdefault(path.stem, f"benchmark source ({path.name})")
    return units


def example_inventory() -> dict[str, str]:
    """Inventory of shipped examples: {anchor: description}.

    Anchor is the path relative to examples/ (``link_sample.cpp``,
    ``lua/sine.lua``, ``logic/fanout.yaml``). The Zephyr embedded sample
    is documented as a single unit anchored ``zephyr/`` covering its
    whole tree (firmware sample, not per-build-file entries).
    """
    units: dict[str, str] = {}
    if EXAMPLES_DIR.is_dir():
        for path in sorted(EXAMPLES_DIR.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(EXAMPLES_DIR)
            if any(part in EXAMPLE_EXCLUDE_DIRS for part in rel.parts):
                continue
            if rel.parts[0] == "zephyr":
                units.setdefault("zephyr/", "Zephyr embedded sample (socket + uart)")
            else:
                units.setdefault(rel.as_posix(), f"example file ({rel.as_posix()})")
    return units


# --------------------------------------------------------------------------
# Catalogs (level-2 headings in the catalog pages)
# --------------------------------------------------------------------------

def catalog_anchors(pages_dir: Path) -> list[str]:
    """All level-2 ``## <text>`` headings across ``*.md`` files in a dir."""
    anchors: list[str] = []
    if pages_dir.is_dir():
        for path in sorted(pages_dir.glob("*.md")):
            text = path.read_text(encoding="utf-8")
            for match in re.finditer(r"(?m)^## ([^\n]+?)\s*$", text):
                anchors.append((path.name, match.group(1).strip()))
    return anchors


def anchor_matches(heading: str, unit: str) -> bool:
    """Heading text vs. inventory unit identity (exact anchor equality)."""
    return heading == unit


TEST_HEADING_RE = re.compile(r"^test_\w+$")
SUPPORT_FILE_RE = re.compile(r"^[\w.-]+\.(?:py|sh|hpp)$")
PROBE_RE = re.compile(r"^assert_fail_probe$")
BENCH_HEADING_RE = re.compile(r"^bench_\w+$")
EXAMPLE_HEADING_RE = re.compile(
    r"^(?:zephyr/|[\w.-]+\.(?:cpp|py|lua|yaml)(?:/[\w.-]+)*"
    r"(?:/(?:[\w.-]+))*)$"
)


def audit_category(
    name: str,
    inventory: dict[str, str],
    anchors: list[tuple[str, str]],
    candidate_re: re.Pattern[str],
) -> bool:
    """One category audit. Returns True when zero missing and zero ghosts."""
    covered = {unit: False for unit in inventory}
    ghosts: list[tuple[str, str]] = []

    for page, heading in anchors:
        if heading in inventory:
            covered[heading] = True
            continue
        # Heading looks like a catalog entry but matches nothing in the
        # inventory -> ghost (renamed/stale entry).
        if candidate_re.match(heading):
            ghosts.append((page, heading))

    missing = [unit for unit, seen in covered.items() if not seen]
    ok = not missing and not ghosts
    total = len(inventory)

    if ok:
        print(f"{name}: {total}/{total} covered")
        return True

    print(f"{name}: {total - len(missing)}/{total} covered -- GAPS FOUND")
    for unit in missing:
        print(f"  MISSING  {name[:-1]} entry: ## {unit}  ({inventory[unit]})")
    for page, heading in ghosts:
        print(f"  GHOST    anchor '## {heading}' in {page} matches no inventory item")
    return False


def main(argv: list[str]) -> int:
    categories = {
        "tests": lambda: audit_category(
            "tests",
            test_inventory(),
            catalog_anchors(DOCS_TESTS),
            re.compile(r"^(?:test_\w+$|assert_fail_probe$|[\w.-]+\.(?:py|sh|hpp)$)"),
        ),
        "benchmarks": lambda: audit_category(
            "benchmarks",
            benchmark_inventory(),
            catalog_anchors(DOCS_BENCHMARKS),
            BENCH_HEADING_RE,
        ),
        "examples": lambda: audit_category(
            "examples",
            example_inventory(),
            catalog_anchors(DOCS_EXAMPLES),
            EXAMPLE_HEADING_RE,
        ),
    }

    wanted = argv[1:] or list(categories)
    unknown = [arg for arg in wanted if arg not in categories]
    if unknown:
        fail(f"unknown category: {', '.join(unknown)} (choose from: {', '.join(categories)})")
        return 2

    ok = True
    print("WEngine catalog completeness audit")
    print(f"  repo root: {REPO_ROOT}")
    for name in wanted:
        ok = categories[name]() and ok

    if ok:
        print("AUDIT PASSED: zero missing entries, zero ghost anchors")
        return 0
    print("AUDIT FAILED: catalog gaps listed above", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
