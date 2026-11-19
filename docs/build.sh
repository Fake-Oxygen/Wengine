#!/usr/bin/env bash
# Build the WEngine documentation suite (BC2, contracts/docs-build.md):
#
#   1. check prerequisites (naming anything missing, loud failure)
#   2. doxygen docs/Doxyfile           -> XML in build/docs/doxygen-xml/
#   3. sphinx-build -b html docs build/docs/html
#
# All generated output is strictly out-of-tree (BC3): nothing is written
# inside the source tree. Engine builds never require this script; it is
# invoked on demand only (research R5). Doxygen's undocumented-entity
# warnings stream to the terminal during step 2 — they are the SC-003
# coverage audit input, not build noise.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# --- Prerequisite checks -----------------------------------------------------
missing=()

if command -v doxygen >/dev/null 2>&1; then
    dox_version="$(doxygen --version | head -n1 | grep -oE '^[0-9]+(\.[0-9]+)+' || true)"
    if [ -n "$dox_version" ]; then
        lowest="$(printf '%s\n' "1.9.2" "$dox_version" | sort -V | head -n1)"
        if [ "$lowest" != "1.9.2" ]; then
            missing+=("doxygen >= 1.9.2 (Breathe requirement; found ${dox_version})")
        fi
    else
        missing+=("doxygen (version could not be parsed)")
    fi
else
    missing+=("doxygen (>= 1.9.2; system package)")
fi

command -v sphinx-build >/dev/null 2>&1 \
    || missing+=("sphinx-build (pip install -r docs/requirements.txt)")
command -v python3 >/dev/null 2>&1 \
    || missing+=("python3 (>= 3.9)")

# Python modules of the pinned docs toolchain, checked by import so the
# error names the exact missing module and the fix.
python3 - <<'EOF' || missing+=("python modules (pip install -r docs/requirements.txt)")
import sys

required = {
    "sphinx": "9.1.1",
    "myst_parser": "5.1.0",
    "breathe": "5.1.0",
    "furo": "2025.12.19",
    "sphinx_copybutton": "0.5.2",
}

absent = []
resolved = []
try:
    from importlib import metadata

    for module, pin in required.items():
        try:
            version = metadata.version(module)
        except metadata.PackageNotFoundError:
            absent.append(f"{module}=={pin}")
            continue
        resolved.append(f"{module} {version}")
except Exception as exc:  # pragma: no cover - defensive
    print(f"python module check failed: {exc}", file=sys.stderr)
    sys.exit(1)

if absent:
    print(
        "missing python modules: " + ", ".join(absent)
        + " (pip install -r docs/requirements.txt)",
        file=sys.stderr,
    )
    sys.exit(1)

print("python modules: " + ", ".join(resolved))
EOF

if [ "${#missing[@]}" -gt 0 ]; then
    echo "docs/build.sh: missing prerequisites:" >&2
    for entry in "${missing[@]}"; do
        echo "  - $entry" >&2
    done
    echo "Install them, then re-run docs/build.sh." >&2
    exit 1
fi

# --- Step 1: Doxygen XML extraction ------------------------------------------
# doxygen creates only the output leaf, not parent directories.
mkdir -p build/docs/doxygen-xml
echo "==> doxygen: extracting include/wengine/ -> build/docs/doxygen-xml/"
echo "    (EXTRACT_ALL=YES renders every public entity, including the"
echo "     not-yet-documented ones — nothing undocumented can hide)"
doxygen docs/Doxyfile

if [ ! -f build/docs/doxygen-xml/index.xml ]; then
    echo "docs/build.sh: doxygen produced no XML (expected build/docs/doxygen-xml/index.xml)" >&2
    exit 1
fi

# --- Step 2: API coverage audit (SC-003) -------------------------------------
# Two mechanisms, both must pass at delivery:
#
# (a) Doxygen's own warnings in the rendering pass below. These fire only
#     for REAL defects (broken comment blocks, `@file <text>` misuse where
#     trailing text is parsed as a filename, unknown commands) —
#     undocumented entities never warn here (see below), so every warning
#     is actionable.
# (b) `docs/check_api_coverage.py`: parses the deliverable XML and
#     requires a non-empty brief on every public entity, docs on every
#     function parameter, and a role paragraph on every file.
#
# Why not WARN_IF_UNDOCUMENTED: Doxygen auto-disables it whenever
# EXTRACT_ALL=YES (our rendering mode — gaps must stay visible, never
# silently disappear), and the EXTRACT_ALL=NO fallback pass emits false
# "parameters/return not documented" positives on correctly documented
# entities as soon as a `/** @file */` block is present (reproduced with
# the contract's verbatim DC2 example; the `@file` form itself is
# mandatory per DC1). Warning text therefore cannot distinguish
# documented from undocumented — the XML can. Until the in-code comment
# pass completes this step reports the gap loudly.
echo "==> doxygen: API coverage audit (undocumented public entities)"

# (a) Rendering-pass warnings are real defects only — fail on any.
dox_warnings="$(doxygen docs/Doxyfile 2>&1 | grep -E ' warning: ' || true)"
if [ -n "$dox_warnings" ]; then
    printf '%s\n' "$dox_warnings" | head -15
    echo "docs/build.sh: doxygen reported warnings (see above)" >&2
    echo "    (broken comment blocks, @file misuse, or unknown commands —" >&2
    echo "    every warning here is actionable; fix and re-run)" >&2
    exit 1
fi

# (b) XML coverage audit: every public entity documented (SC-003).
if ! python3 docs/check_api_coverage.py; then
    echo "docs/build.sh: API coverage audit found undocumented entities (see above)" >&2
    echo "    (SC-003 target is zero documented gaps)" >&2
    exit 1
fi
echo "    API coverage: zero undocumented public entities"

# --- Step 3: Sphinx HTML build ------------------------------------------------
echo "==> sphinx-build: rendering docs/ -> build/docs/html/"
sphinx-build -b html docs build/docs/html

if [ ! -f build/docs/html/index.html ]; then
    echo "docs/build.sh: sphinx produced no site (expected build/docs/html/index.html)" >&2
    exit 1
fi

echo "==> documentation suite built:"
echo "    build/docs/html/index.html"
