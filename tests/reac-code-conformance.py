#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# "We need the error codes and not only messages" (operator, 2026-09-17). Two checks:
#
#  1. Every token declared in src/reac_code.h's REAC_CODE_LIST is USED somewhere in
#     src/*.c through reac_code_emit — a declared code nobody ever emits is exactly the
#     kind of thing a conformance test exists to catch (an unreachable branch of a
#     vocabulary that is supposed to be closed and complete).
#  2. A FLOOR on how many refusal/failure-shaped `fprintf(stderr, "reac-pw: ...)` lines
#     still bypass reac_code_emit — migration here is proportionate (spec §2), not
#     exhaustive, so this is not a zero-tolerance gate. It is a RATCHET: the floor may
#     only fall. A change that adds a new bare refusal/failure line, or fails to migrate
#     one that was already counted, pushes the count over the floor and fails the build.
#
# Run directly: `python3 tests/reac-code-conformance.py [src-dir] [reac_code.h]`.

import os
import re
import sys
from pathlib import Path

# The floor as of 2026-09-17 (28 bare refusal/failure-shaped lines left, all named
# owed in the spec's proportionate-scope note). LOWER THIS when a line migrates;
# never raise it to make a new one fit. Covers reac-pw's own src/*.c always, and
# libreac's src/*.c + transport/src/*.c too when the sibling checkout is found
# (see find_libreac() below) — the combined count, so a line moved from one repo's
# bare-fprintf into the other's would not quietly duck the ratchet.
FLOOR = 31

REFUSAL_WORDS = re.compile(r'REFUSED|FATAL|failed|FAILED|could not|COULD NOT')
FPRINTF_START = re.compile(r'fprintf\(stderr,\s*"reac-pw:')
# libreac's own prog tags (reac_master.c/reac_pacer.c/reac_slave.c/reac_ifscan.c) —
# broader than reac-pw's single "reac-pw:" prefix, since libreac's fprintf lines
# name the module, not the daemon.
FPRINTF_START_LIBREAC = re.compile(
    r'fprintf\(stderr,\s*"(reac-pw|reac-pacer|reac_pacer|reac_slave|reac-master|reac_master):')
FPRINTF_CALL = re.compile(r'\bfprintf\(\s*stderr\s*,')
CODE_EMIT_START = re.compile(r'reac_code_emit\(stderr,\s*"[a-z_-]+"')
TOKEN_DEF = re.compile(r'X\(\s*(RC_[A-Z_]+)\s*,\s*"([A-Z_]+)"\s*\)')


def find_libreac(reac_pw_root):
    """The sibling libreac checkout, when present: LIBREAC_SRCDIR if set, else the
    conventional sibling beside this repo (tools/r1-build-test.sh's own convention
    for the same lookup). Recognized by shipping include/reac/reac_code.h — the
    shared vocabulary header (docs/design/specs/
    2026-09-17-tunables-api-and-shared-refusal-codes.md) — not just by existing:
    an older libreac clone with no such header has nothing this scan can join.
    Returns None, silently, when neither is found: this ratchet must not require
    the sibling checkout to build or test reac-pw on its own."""
    env_dir = os.environ.get('LIBREAC_SRCDIR')
    candidates = [Path(env_dir)] if env_dir else [
        reac_pw_root.parent / 'libreac',
        reac_pw_root.parent / 'libreac-wt-knobs',
    ]
    # When more than one sibling carries the header, the NEWEST header wins (mtime), never the
    # first name in the list: a stale ../libreac beside a fresher lane worktree must not be
    # the tree this scan joins (review of the 2026-09-17 lane).
    found = [c for c in candidates if (c / 'include' / 'reac' / 'reac_code.h').exists()]
    if not found:
        return None
    return max(found, key=lambda c: (c / 'include' / 'reac' / 'reac_code.h').stat().st_mtime)


def statement_text(lines, start_idx, max_lines=8):
    """Concatenate a C statement's string-literal lines starting at start_idx,
    stopping at the terminating ';' or after max_lines -- good enough for the
    multi-line fprintf/reac_code_emit calls this file is full of."""
    buf = []
    for k in range(start_idx, min(start_idx + max_lines, len(lines))):
        buf.append(lines[k])
        if ';' in lines[k]:
            break
    return ''.join(buf)


def declared_tokens(code_h_path):
    text = code_h_path.read_text()
    return {name: token for name, token in TOKEN_DEF.findall(text)}


def used_tokens(src_files):
    used = set()
    for path in src_files:
        text = path.read_text()
        for m in re.finditer(r'reac_code_emit\([^,]+,\s*"[^"]+",\s*(RC_[A-Z_]+)', text):
            used.add(m.group(1))
    return used


def bare_refusal_lines(src_files, start_pattern=FPRINTF_START):
    hits = []
    for path in src_files:
        lines = path.read_text().splitlines(keepends=True)
        for i, line in enumerate(lines):
            # Match on the JOINED statement, never the physical line: a refusal written as
            # `fprintf(stderr,\n\t"reac-pw: FATAL ...")` -- the common split style -- shares no
            # line between the call and the prefix and slipped past a per-line match
            # (review of this lane, 2026-09-17: three such lines in main.c were not counted).
            if not FPRINTF_CALL.search(line):
                continue
            stmt = statement_text(lines, i)
            if CODE_EMIT_START.search(stmt) or not start_pattern.search(stmt):
                continue
            if REFUSAL_WORDS.search(stmt):
                hits.append(f'{path.name}:{i + 1}')
    return hits


def main(argv):
    src_dir = Path(argv[1]) if len(argv) > 1 else Path('src')
    code_h = Path(argv[2]) if len(argv) > 2 else src_dir / 'reac_code.h'

    if not code_h.exists():
        print(f'{code_h} not found -- the scan is broken, not the code', file=sys.stderr)
        return 1

    declared = declared_tokens(code_h)
    if not declared:
        print(f'found zero tokens in {code_h} -- the scan is broken, not the header',
              file=sys.stderr)
        return 1

    src_files = sorted(src_dir.glob('*.c'))
    if not src_files:
        print(f'found zero .c files under {src_dir} -- the scan is broken, not the tree',
              file=sys.stderr)
        return 1

    all_src_files = list(src_files)
    hits = bare_refusal_lines(src_files, FPRINTF_START)

    libreac_root = find_libreac(src_dir.resolve().parent)
    joined_note = ''
    if libreac_root:
        libreac_code_h = libreac_root / 'include' / 'reac' / 'reac_code.h'
        libreac_src_files = sorted((libreac_root / 'src').glob('*.c')) + \
            sorted((libreac_root / 'transport' / 'src').glob('*.c'))
        declared.update(declared_tokens(libreac_code_h))
        all_src_files += libreac_src_files
        hits += bare_refusal_lines(libreac_src_files, FPRINTF_START_LIBREAC)
        joined_note = (f' + libreac sibling at {libreac_root} '
                        f'({len(libreac_src_files)} .c files joined)')

    used = used_tokens(all_src_files)
    unused = sorted(set(declared) - used)
    if unused:
        print(f'{len(unused)} declared code(s) are never emitted: {unused}', file=sys.stderr)
        return 1

    if len(hits) > FLOOR:
        print(f'{len(hits)} bare refusal/failure fprintf lines bypass reac_code_emit, '
              f'over the floor of {FLOOR}:', file=sys.stderr)
        for h in hits:
            print(f'  {h}', file=sys.stderr)
        return 1

    print(f'OK: {len(declared)} codes all emitted at least once; '
          f'{len(hits)} bare refusal line(s) left (floor {FLOOR}){joined_note}.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
