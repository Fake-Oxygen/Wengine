# Sphinx configuration for the WEngine documentation suite.
#
# The site's sources are MyST Markdown (docs/**/*.md). Doxygen extracts
# include/wengine/** to XML (docs/Doxyfile) and Breathe renders that XML
# inside the per-module API pages (docs/api/). Output is out-of-tree at
# build/docs/html/ (BC3). Run via docs/build.sh from the repository root.

project = "WEngine"
author = "WEngine contributors"
copyright = "2026, WEngine contributors"

# --- Extensions ------------------------------------------------------------
# myst_parser      MyST Markdown sources (the existing topic guides are
#                  CommonMark and render unmodified).
# breathe          Doxygen XML -> Sphinx API rendering (docs/api/ pages).
# sphinx_copybutton  one-click copies on every code block.
extensions = [
    "myst_parser",
    "breathe",
    "sphinx_copybutton",
]

# --- Sources ---------------------------------------------------------------
source_suffix = ".md"
root_doc = "index"

# --- Theme -----------------------------------------------------------------
# furo: audience-oriented sidebar, per-page outline, built-in search
# (research R2).
html_theme = "furo"
html_title = "WEngine documentation"

# --- Breathe / Doxygen bridge -----------------------------------------------
# XML path is relative to this file's directory; the Doxyfile writes its
# XML there (XML_OUTPUT="." inside OUTPUT_DIRECTORY=build/docs/doxygen-xml).
breathe_projects = {"wengine": "../build/docs/doxygen-xml"}
breathe_default_project = "wengine"
breathe_default_members = ()

# --- Domain -----------------------------------------------------------------
# C++ is the site's primary language; the API reference lives in the cpp
# domain.
primary_domain = "cpp"
highlight_language = "cpp"

# --- MyST --------------------------------------------------------------------
# Explicit anchors for headings up to level 3 make catalog entries
# (`## test_<name>`, `## bench_<name>`) deep-linkable (NC4).
myst_heading_anchors = 3

# --- Warning policy ------------------------------------------------------------
# BC4: the build must complete without errors; warnings are allowed only
# from a recorded allow-list. `duplicate_declaration.cpp` is allow-listed
# BY DESIGN: every API module page pairs curated Key-entity directives
# with a per-module Complete listing (`doxygennamespace`), so each key
# entity registers in the C++ domain index twice. That duplication is
# structural to the mandated page shape, not a content error — the real
# completeness gate is the XML audit (`docs/check_api_coverage.py`), not
# the domain index.
suppress_warnings = ["duplicate_declaration.cpp"]
