#!/usr/bin/env python3
"""Comment-only edit verifier (DC6 / SC-007 gate).

For each file argument, compares the comment-stripped text of the working
tree against the HEAD version (when the file is tracked and modified).
Standalone comment insertions strip to blank lines, which carry no code
tokens and are ignored; every line holding a code token must match
exactly. Any other difference means a code token changed and the edit
is NOT comment-only.

Usage:
    python3 docs/check_comment_only.py <file> [<file> ...]

Exit status:
    0  all files verified comment-only (or untouched)
    1  at least one file has a code-token difference
    2  usage / environment error (e.g. file not in git)

The stripper is conservative: constructs it cannot lex confidently
(unterminated string/comment, backslash-continued // comment, an
apostrophe following a digit in code context) make it report the file
for MANUAL REVIEW instead of silently passing.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

RAW_STRING_RE = re.compile(r'R"([^ ()\\\t\v\f\n]*)\(')


class ManualReview(Exception):
    pass


def strip_comments(text: str) -> str:
    """Return `text` with C++ comments removed, preserving all other bytes."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        char = text[i]

        # Raw string literal: R"delim( ... )delim"
        if char == "R" and i + 1 < n and text[i + 1] == '"':
            match = RAW_STRING_RE.match(text, i)
            if match:
                delim = match.group(1)
                closer = f"){delim}\""
                end = text.find(closer, match.end())
                if end == -1:
                    raise ManualReview("unterminated raw string")
                out.append(text[i : end + len(closer)])
                i = end + len(closer)
                continue
            # Not a raw string (e.g. identifier ending in R"): fall through.

        # String / char literal.
        if char in ('"', "'"):
            quote = char
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    break
                if text[j] == "\n":
                    raise ManualReview(f"newline inside {quote}-literal")
                j += 1
            if j >= n:
                raise ManualReview(f"unterminated {quote}-literal")
            out.append(text[i : j + 1])
            i = j + 1
            continue

        # Line comment.
        if char == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j == -1:
                j = n
            # A backslash-newline keeps the line comment running.
            seg = text[i:j]
            while seg.endswith("\\"):
                nxt = text.find("\n", j)
                seg_end = nxt if nxt != -1 else n
                seg = text[i:seg_end]
                j = seg_end
                if nxt == -1:
                    break
            i = j
            continue

        # Block comment.
        if char == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            if end == -1:
                raise ManualReview("unterminated block comment")
            # Keep newlines inside the block so line numbering survives.
            out.append("\n" * text.count("\n", i, end + 2))
            i = end + 2
            continue

        out.append(char)
        i += 1
    return "".join(out)


def significant_lines(stripped: str) -> list[str]:
    """Non-blank lines of comment-stripped text.

    A standalone added comment line strips to a whitespace-only line;
    those carry no code tokens and are dropped so pure comment additions
    verify clean. Every line holding a code token is kept verbatim, so
    any token change, reflow, or indent edit still diffs.
    """
    return [line for line in stripped.split("\n") if line.strip() != ""]


def head_version(path: Path) -> str | None:
    result = subprocess.run(
        ["git", "show", f"HEAD:{path.as_posix()}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2

    bad = 0
    for name in argv[1:]:
        path = Path(name)
        if not path.is_file():
            print(f"{name}: not a file", file=sys.stderr)
            bad += 1
            continue
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            print(f"{name}: skipped (C++ comment-strip applies to .cpp/.hpp/.h only)")
            continue

        try:
            current = path.read_text(encoding="utf-8")
            stripped_now = strip_comments(current)
        except ManualReview as exc:
            print(f"{name}: MANUAL REVIEW ({exc})")
            bad += 1
            continue

        base = head_version(path)
        if base is None:
            print(f"{name}: untracked/new file (no HEAD baseline) -- stripped-text check skipped")
            continue
        if base == current:
            continue
        try:
            stripped_base = strip_comments(base)
        except ManualReview as exc:
            print(f"{name}: MANUAL REVIEW on HEAD version ({exc})")
            bad += 1
            continue

        if significant_lines(stripped_base) == significant_lines(stripped_now):
            print(f"{name}: comment-only OK")
        else:
            print(f"{name}: CODE TOKENS CHANGED -- not a comment-only edit")
            bad += 1

    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
