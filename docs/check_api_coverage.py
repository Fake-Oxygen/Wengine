#!/usr/bin/env python3
"""API documentation coverage audit for the WEngine documentation suite.

Checks that every public entity under ``include/wengine/`` carries
Doxygen contract documentation (contracts/
doxygen-comment-conventions.md, SC-003). This is the SC-003 gate:
"auditable by construction".

Why XML, not warnings: Doxygen 1.9.8 auto-disables
``WARN_IF_UNDOCUMENTED`` whenever ``EXTRACT_ALL=YES`` (our rendering
mode, which must show undocumented entities rather than hide them),
and the ``EXTRACT_ALL=NO`` fallback pass emits false "parameters/return
not documented" positives on correctly documented entities as soon as a
``/** @file */`` block is present (reproduced with the contract's
verbatim DC2 example). Warning text therefore cannot distinguish
documented from undocumented. The XML of the real deliverable pass can:
documented entities carry non-empty description elements.

Mechanism (stdlib only):
  * parse every ``build/docs/doxygen-xml/*.xml`` (the ``EXTRACT_ALL=YES``
    deliverable pass — run ``doxygen docs/Doxyfile`` first);
  * file compounds (``kind=file``) whose location is under
    ``include/wengine/``: require a non-empty brief OR detailed
    description (the DC1 ``@file`` role paragraph lands in the detailed
    description);
  * every ``prot=public`` member: require a non-empty ``<brief>``
    (explicit ``@brief``, DC2);
  * every public function parameter with a declared name: require a
    non-empty parameter description (``@param``, DC2);
  * enum values are out of scope (several share one source line, so
    own-line blocks are impossible without reflowing code; the parent
    enum's ``@note`` names and describes them) — the parent enum itself
    is in scope.

Behavior:
  * exit 0 listing "N/N covered".
  * exit 1 listing every gap (file, entity, missing field).

Usage: ``python3 docs/check_api_coverage.py [xml-dir]``
"""

from __future__ import annotations

import glob
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def text(element: ET.Element | None) -> str:
    if element is None:
        return ""
    return "".join(element.itertext()).strip()


def member_source(member: ET.Element, fallback: str) -> str:
    """The header a member is actually declared in.

    Namespace compounds span many headers (their compound location names
    only the first), so every member is attributed via its OWN location
    element — otherwise gaps are reported under the wrong file.
    """
    location = member.find("location")
    if location is not None and location.get("file"):
        return location.get("file", "")
    return fallback


def documented_params(member: ET.Element) -> set[str]:
    """Names carrying a non-empty description in the parameter list.

    Doxygen 1.9.8 never emits text in ``<param><briefdescription>``
    (it is always empty); ``@param`` docs land in
    ``detaileddescription/parameterlist/parameteritem`` instead, with the
    name in ``parameternamelist/parametername``. Checking the ``<param>``
    brief therefore fails every parameter project-wide, documented or
    not — the parameter list is the authoritative source.
    """
    documented: set[str] = set()
    detail = member.find("detaileddescription")
    if detail is None:
        return documented
    for item in detail.iter("parameteritem"):
        description = item.find("parameterdescription")
        text = "".join(description.itertext()).strip() if description is not None else ""
        if not text:
            continue
        names = item.find("parameternamelist")
        if names is None:
            continue
        for pname in names.iter("parametername"):
            if pname.text and pname.text.strip():
                documented.add(pname.text.strip())
    return documented


def declared_params(member: ET.Element) -> list[str]:
    """Declared parameter names (``void`` and unnamed slots excluded)."""
    names: list[str] = []
    for param in member.findall("param"):
        declname = (param.findtext("declname") or "").strip()
        if declname:
            names.append(declname)
    return names


def main(argv: list[str]) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    xml_dir = Path(argv[1]) if len(argv) > 1 else repo_root / "build/docs/doxygen-xml"
    if not xml_dir.is_dir():
        print(
            f"check_api_coverage.py: no XML dir {xml_dir} "
            "(run `doxygen docs/Doxyfile` first)",
            file=sys.stderr,
        )
        return 2

    total = 0
    gaps: list[str] = []

    for path in sorted(glob.glob(str(xml_dir / "*.xml"))):
        if os.path.basename(path) == "index.xml":
            continue
        try:
            tree = ET.parse(path)
        except ET.ParseError as exc:
            print(f"check_api_coverage.py: cannot parse {path}: {exc}", file=sys.stderr)
            return 2
        for compound in tree.getroot().iter("compounddef"):
            kind = compound.get("kind", "")
            if kind == "dir":
                continue  # directory index, not a documented entity
            location = compound.find("location")
            compound_source = location.get("file", "") if location is not None else ""
            if "include/wengine" not in compound_source.replace(os.sep, "/"):
                continue
            name = compound.findtext("compoundname") or "?"
            if kind == "file":
                total += 1
                brief = text(compound.find("briefdescription"))
                detail = text(compound.find("detaileddescription"))
                if not brief and not detail:
                    gaps.append(f"{compound_source}: file has no @file role paragraph")
                continue
            if kind != "namespace":
                # class/struct/union/interface: the type itself is an entity —
                # but only when public (private nested types are
                # implementation details; the compound carries prot).
                if compound.get("prot", "public") != "public":
                    continue
                total += 1
                if not text(compound.find("briefdescription")):
                    gaps.append(f"{compound_source}: {kind} {name} has no @brief")
            for member in compound.iter("memberdef"):
                if member.get("prot") != "public":
                    continue
                source = member_source(member, compound_source)
                # Members of detail namespaces live in headers outside
                # include/wengine (or in .cpp TUs) — out of scope.
                if "include/wengine" not in source.replace(os.sep, "/"):
                    continue
                member_kind = member.get("kind", "")
                member_name = member.findtext("name") or "?"
                total += 1
                if not text(member.find("briefdescription")):
                    gaps.append(f"{source}: {name}::{member_name} ({member_kind}) has no @brief")
                    continue
                if member_kind == "function":
                    have = documented_params(member)
                    for pname in declared_params(member):
                        if pname not in have:
                            gaps.append(
                                f"{source}: {name}::{member_name} "
                                f"param '{pname}' has no @param doc"
                            )

    covered = total - len({g.split(" has no ")[0] for g in gaps})
    print(f"API coverage: {total} entities checked")
    if not gaps:
        print(f"API coverage: {total}/{total} covered")
        print("API COVERAGE PASSED: every public entity documented")
        return 0
    print(f"API coverage: {covered}/{total} covered -- GAPS FOUND")
    for gap in gaps:
        print(f"  MISSING  {gap}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
