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
#  3. ONE HOME (libreac's docs/design/specs/2026-09-29-shared-code-has-one-home.md,
#     operator 2026-09-29: "all the functions and code that can be shared, must be
#     shared"). libreac's <reac/reac_code.h> is the only code list and <reac/reac_cfg.h>
#     the only spelling of the cfg vocabulary; the tap read and the qdisc dump are
#     libreac's. So: src/reac_code.h carries no X list of its own; no string literal in
#     src/ equals a reac_cfg.h value (the "none" convention aside); no src/ file reads
#     PACKET_AUXDATA or sends RTM_GETQDISC. Each arm has a planted copy it must refuse.
#
# Run directly: `python3 tests/reac-code-conformance.py <src-dir> <src/reac_code.h>
# <libreac include dir>` (meson passes the include dir libreac_dep resolved to).

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
# 2026-09-20 (#106): the two roster-node failures now emit E_ROSTER_NODE, so the floor
# falls 31 -> 30. It only ever falls.
FLOOR = 30

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


def declared_tokens(code_h_path, half=None):
    """Tokens of libreac's REAC_CODE_LIST, or of one half of it ('DAEMON' / 'LIBRARY')."""
    text = code_h_path.read_text()
    if half:
        m = re.search(r'#define REAC_CODE_LIST_' + half + r'\(X\)(.*?)\n\s*\n', text, re.S)
        text = m.group(1) if m else ''
    return {name: token for name, token in TOKEN_DEF.findall(text)}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


STRING_LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CFG_STRING_DEF = re.compile(r'^#define\s+([A-Z_0-9]+)\s+"([^"]*)"', re.M)
LIBREAC_ONLY = [
    (re.compile(r'\bPACKET_AUXDATA\b'), 'reads PACKET_AUXDATA: the tap read is reac_topo_tap_read()'),
    (re.compile(r'\bRTM_GETQDISC\b'), 'dumps RTM_GETQDISC: the qdisc dump is reac_etf_qdisc_dump()'),
]


def one_home_offences(src_dir, code_h, cfg_h):
    """Arm 3. Returns a list of 'file:line: why' strings."""
    out = []
    local = strip_comments(code_h.read_text())
    if TOKEN_DEF.search(local) or '#include <reac/reac_code.h>' not in local:
        out.append(f'{code_h.name}: carries a code list of its own; the list is '
                   f'libreac\'s <reac/reac_code.h> (add a code to its DAEMON half)')
    # A value reac_cfg.h gives to more than one name ("applied" is rate's and role's)
    # is a word of the estate, like "none", and another family (reac.headamp.state)
    # may use it too; a value with ONE name is that name's spelling and nobody else's.
    defs = CFG_STRING_DEF.findall(cfg_h.read_text())
    count = {}
    for _, v in defs:
        count[v] = count.get(v, 0) + 1
    values = {v: n for n, v in defs if v not in ('', 'none') and count[v] == 1}
    for path in sorted(src_dir.glob('*.[ch]')):
        text = strip_comments(path.read_text())
        for i, line in enumerate(text.splitlines(), 1):
            for m in STRING_LIT.finditer(line):
                if m.group(1) in values:
                    out.append(f'{path.name}:{i}: "{m.group(1)}" is {values[m.group(1)]} '
                               f'in <reac/reac_cfg.h>; name it, never spell it')
            for rx, why in LIBREAC_ONLY:
                if rx.search(line):
                    out.append(f'{path.name}:{i}: {why}')
    return out


def planted_one_home(cfg_h):
    """The detector must refuse each planted copy and pass the planted good tree;
    otherwise a clean result is a broken search, not a clean tree."""
    import tempfile
    defs = CFG_STRING_DEF.findall(cfg_h.read_text())
    vals = [v for _, v in defs]
    val = next(v for v in vals if v not in ('', 'none') and vals.count(v) == 1)
    good_code = '#include <reac/reac_code.h>\n'
    cases = {
        'good': (good_code, 'const char *k = REAC_CFG_RATE_PROP;\n', 0),
        'list': ('#define REAC_CODE_LIST(X) X(RC_E_A, "E_A")\n', '', 1),
        'literal': (good_code, f'const char *k = "{val}";\n', 1),
        'auxdata': (good_code, 'int t = PACKET_AUXDATA;\n', 1),
        'getqdisc': (good_code, 'int t = RTM_GETQDISC;\n', 1),
    }
    for name, (code, c, want) in cases.items():
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'reac_code.h').write_text(code)
            (d / 'x.c').write_text('/* "' + val + '" in a comment is prose */\n' + c)
            got = len(one_home_offences(d, d / 'reac_code.h', cfg_h))
            if (got > 0) != bool(want):
                return f'the one-home arm {"passed" if want else "refused"} its planted ' \
                       f'{name} tree -- the arm is broken'
    return None


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
    inc = Path(argv[3]) if len(argv) > 3 else None
    if inc is None or not (inc / 'reac' / 'reac_code.h').exists():
        print(f'no <reac/reac_code.h> under {inc} -- the scan cannot read the one list',
              file=sys.stderr)
        return 1
    lib_code_h = inc / 'reac' / 'reac_code.h'
    cfg_h = inc / 'reac' / 'reac_cfg.h'

    if not code_h.exists():
        print(f'{code_h} not found -- the scan is broken, not the code', file=sys.stderr)
        return 1

    broken = planted_one_home(cfg_h)
    if broken:
        print(broken, file=sys.stderr)
        return 1
    offences = one_home_offences(src_dir, code_h, cfg_h)
    if offences:
        print(f'{len(offences)} copy/copies of what libreac declares or does:', file=sys.stderr)
        for o in offences:
            print(f'  {o}', file=sys.stderr)
        return 1

    declared = declared_tokens(lib_code_h, 'DAEMON')
    if not declared:
        print(f'found zero DAEMON tokens in {lib_code_h} -- the scan is broken, not the header',
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
        declared.update(declared_tokens(libreac_code_h, 'LIBRARY'))
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

    print(f'OK: one home (no list, literal, tap read or qdisc dump copied); '
          f'{len(declared)} codes all emitted at least once; '
          f'{len(hits)} bare refusal line(s) left (floor {FLOOR}){joined_note}.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
