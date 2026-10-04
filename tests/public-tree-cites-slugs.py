#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PUBLIC TREE CITES INTERNAL DOCUMENTS BY SLUG, AND THE README BUILDS NOTHING.
#
# Specs, notes, audits and rig files are internal and live in the private ops
# repository, not here. Code, tests, packaging and the user docs may still cite one, but
# by its bare slug (`2026-09-16-segments-and-roles-are-autodetected`, no directory, no
# `.md`), so a citation is never a path that resolves to nothing in a public checkout.
# Three checks, each finding named with its file and line:
#
#  1. No file names a path under the internal-doc directories (design, audits).
#  2. No file names a dated document by its filename (`<date>-<slug>.md`).
#  3. README.md is the pitch and the install: no build command in its code blocks, and
#     BUILDING.md exists and carries the build (build instructions are public, just not on
#     the README).
#
# The internal directories themselves are skipped while they are still in this tree.
#
# Run directly: `python3 tests/public-tree-cites-slugs.py [source-dir]`.

import os
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)

# Joined at run time so this file does not match its own patterns.
DOCS = "docs" + "/"
INTERNAL_DIRS = (DOCS + "design", DOCS + "audits")
INTERNAL_FILES = (DOCS + "RIG-MASTERS.txt",)
SKIP_DIRS = {".git", "subprojects", "logs", "__pycache__"}

PATH_CITE = re.compile(r"\b" + re.escape(DOCS) + r"(?:design|audits)/")
DATED_MD = re.compile(r"\b\d{4}-\d\d-\d\d-[A-Za-z0-9-]+\." + "md" + r"\b")
BUILD_CMD = re.compile(r"^\s*(?:sudo\s+)?(?:meson|ninja|make|cmake|rpmbuild|pnpm\s+build|"
                       r"tools/build[\w.-]*|packaging/build[\w.-]*)(?:\s|$)")


def public_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel_dir = Path(dirpath).relative_to(ROOT).as_posix()
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        dirnames[:] = sorted(
            d for d in dirnames
            if d not in SKIP_DIRS and not d.startswith("build") and not d.startswith("_build")
            and not (d.startswith(".") and d != ".github")
            and (rel_dir + d) not in INTERNAL_DIRS)
        for f in sorted(filenames):
            rel = rel_dir + f
            if rel in INTERNAL_FILES:
                continue
            yield rel


def text_lines(rel):
    try:
        data = (ROOT / rel).read_bytes()
    except OSError:
        return []
    if b"\0" in data:
        return []
    return data.decode("utf-8", "replace").splitlines()


def main():
    bad = []
    seen = 0
    for rel in public_files():
        seen += 1
        for n, line in enumerate(text_lines(rel), 1):
            if PATH_CITE.search(line):
                bad.append(f"{rel}:{n}: names an internal-doc path; cite the bare slug: {line.strip()}")
            elif DATED_MD.search(line):
                bad.append(f"{rel}:{n}: names a dated document by filename; drop the .md: {line.strip()}")

    readme = text_lines("README.md")
    if not readme:
        bad.append("README.md: missing")
    fenced = False
    for n, line in enumerate(readme, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced and BUILD_CMD.match(line):
            bad.append(f"README.md:{n}: a build command belongs in BUILDING.md: {line.strip()}")

    building = text_lines("BUILDING.md")
    if not any(re.match(r"\s*meson\s+setup\b", l) for l in building):
        bad.append("BUILDING.md: missing, or carries no `meson setup` step")

    for b in bad:
        print("FAIL: " + b)
    if seen == 0:
        print(f"FAIL: no files found under {ROOT}")
        return 1
    if bad:
        print(f"FAIL: {len(bad)} finding(s) in {seen} public files")
        return 1
    print(f"OK: {seen} public files cite internal documents by slug; README builds nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
