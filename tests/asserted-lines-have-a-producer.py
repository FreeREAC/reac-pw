#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# "Let's be precise, let's organise tests to not depend on greps" (operator, 2026-09-17).
# A `grep`/`wait_for` pattern in tests/*.sh asserts that the daemon can print a certain
# line. Nothing checked that the daemon actually CAN — `tests/box-0832-enrols.sh`'s
# `reac.box.width` (unescaped dot, wrong separator: the real property is `reac.box-width`)
# lived for a full spec cycle asserting almost nothing, because it matched too much to
# ever go red on its own subject.
#
# This walks every `grep -q`/`grep -qE`/`grep -c`/`grep -E`/`wait_for`/`wait_for_since`
# quoted pattern in tests/*.sh, and every printf-family format string reac-pw's src/*.c
# can produce (conversions normalized to wildcards), and requires each test pattern to
# share real vocabulary with at least one producer. A pattern that matches NOTHING is
# refused UNLESS it is named, with a reason, in tests/known-debt-grep-patterns.txt — a
# ratchet whose entry count may only fall (FLOOR below).
#
# Proportionate, not a rewrite: most patterns here stay prose, tracked as debt rather
# than migrated to a src/reac_code.h token (spec 2026-09-17-knobs-codes-and-test-
# ratchets.md §3). This script's job is only to make sure every one of them still
# corresponds to something the daemon can actually say.

import re
import sys
from pathlib import Path

# The debt list's entry count as of 2026-09-17. LOWER this when a pattern is migrated
# or given a real producer; never raise it to make room for a new unmapped pattern.
DEBT_FLOOR = 3

PATTERN_CALL = re.compile(
    r'(?:grep\s+-q[E]?|grep\s+-c|grep\s+-E|wait_for(?:_since)?)\s+(?:-\S+\s+)*"((?:[^"\\]|\\.)*)"'
)

FPRINTF_START = re.compile(r'\b(?:fprintf|reac_code_emit|g_warning|fputs)\s*\(')
STRING_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
CONVERSION = re.compile(r'%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|q|L|j|z|t)?[diouxXeEfFgGaAcspn%]')

STOPWORDS = {
    'the', 'a', 'an', 'and', 'or', 'is', 'are', 'was', 'were', 'of', 'on',
    'in', 'at', 'to', 'for', 'with', 'its', 'own', 'that', 'this', 'not',
    'no', 'one', 'off', 'into', 'from', 'over', 'under', 'what', 'when',
    'does', 'never', 'ever', 'still', 'about', 'each', 'every', 'both',
    'was', 'has', 'have', 'been',
    # NOTE: unlike the meson-header checker, domain nouns (segment/box/master/
    # wire/...) are NOT stopwords here -- that check needed to DISTINGUISH one
    # header's subject from another's; this one only needs to know a producer
    # EXISTS for a pattern, and "segment up" sharing "segment" with a hundred
    # producers is exactly the signal that a producer exists.
}

# Patterns that assert the TEST HARNESS's own transcript (its echoed PASS/FAIL/SKIP
# markers, a shell variable dump like `BOXRC=0`) or the SHAPE of a .c source file
# (box-speaks-one-vocabulary.sh's `grep -c "sink_ch" src/main.c`) rather than
# anything the running daemon prints. Neither is a daemon-log producer question, so
# neither is mapped OR debt-listed -- named here, once, instead of silently passed.
SELF_REFERENTIAL = re.compile(
    r'^\^?(PASS|FAIL|SKIP)|^BOXRC=|^const |sink_ch|REAC_ROLE_INTENT'
    r'|^\$[A-Z_][A-Z0-9_]*$'  # a bare shell var: compares captured content to itself
    r"|^suite : \("          # checks meson.build's own suite: tag, not a daemon line
)


def normalize(word):
    for suffix in ('ing', 'ed', 'es', 's'):
        if word.endswith(suffix) and len(word) - len(suffix) >= 3:
            return word[: -len(suffix)]
    return word


def words(text):
    # regex escapes read as near-literal for vocabulary purposes: \[ \] \( \) \. all
    # just delimit real words either side.
    plain = re.sub(r'\\[\[\]().]', ' ', text)
    plain = re.sub(r'\.\*|\.\+|\$\(|\)\$', ' ', plain)
    return {normalize(w) for w in re.split(r'[^a-zA-Z]+', plain.lower())
            if len(w) >= 4 and w not in STOPWORDS}


def extract_patterns(tests_dir):
    found = []
    for path in sorted(tests_dir.glob('*.sh')):
        text = path.read_text()
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in PATTERN_CALL.finditer(line):
                pat = m.group(1)
                if len(pat) < 6:
                    continue  # too short to carry any real assertion
                found.append((path.name, lineno, pat))
    return found


def extract_producers(src_dir):
    producers = []
    for path in sorted(src_dir.glob('*.c')):
        lines = path.read_text().splitlines(keepends=True)
        i = 0
        while i < len(lines):
            if FPRINTF_START.search(lines[i]):
                buf = []
                for k in range(i, min(i + 10, len(lines))):
                    buf.append(lines[k])
                    if ';' in lines[k]:
                        break
                stmt = ''.join(buf)
                lits = STRING_LIT.findall(stmt)
                template = ''.join(lits)
                template = CONVERSION.sub(' ', template)
                producers.append((path.name, i + 1, template))
            i += 1
    return producers


TOKEN_DEF = re.compile(r'X\(\s*RC_[A-Z_]+\s*,\s*"([A-Z_]+)"\s*\)')


def extract_code_tokens(src_dir):
    """reac_code.h's tokens are never a string literal at their fprintf call site
    (the call passes the ENUM; reac_code_token() maps it to text at runtime), so a
    test pattern that matches on the bare token (e.g. 'E_ROOT_REFUSED') needs these
    added as their own one-word producers, or every such test would show as
    unmapped despite reac_code_emit being the thing that prints it."""
    code_h = src_dir / 'reac_code.h'
    if not code_h.exists():
        return []
    text = code_h.read_text()
    return [(code_h.name, 0, tok) for tok in TOKEN_DEF.findall(text)]


def build_producer_wordsets(producers):
    return [(f, l, words(t)) for f, l, t in producers]


def matches(pattern_words, producer_wordsets):
    if not pattern_words:
        return False
    # real overlap with SOME ONE producer, not a coincidence. A pattern with only
    # 1-2 significant words is often `[tag] <one real word>` -- the tag is a
    # test-local interface name that can never appear literally in a producer
    # (it only ever reaches the wire through a %s), so requiring BOTH words
    # would refuse a pattern for carrying a name instead of for lacking a
    # producer. 2+ shared words for anything with more to say; 1 is enough
    # when the pattern itself has at most 2 words to give. Not a majority
    # requirement either way -- prose paraphrases ("chosen by hearing the
    # wire") carry real words a terser producer template legitimately does
    # not repeat.
    need = 1 if len(pattern_words) <= 2 else 2
    best = 0
    for _, _, pw in producer_wordsets:
        if not pw:
            continue
        shared = len(pattern_words & pw)
        best = max(best, shared)
        if best >= need:
            return True
    return False


def load_debt(debt_path):
    debt = {}
    if not debt_path.exists():
        return debt
    for line in debt_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        # format: file:line: reason
        parts = line.split(':', 2)
        if len(parts) != 3:
            continue
        f, l, reason = parts
        debt[(f.strip(), int(l.strip()))] = reason.strip()
    return debt


def main(argv):
    tests_dir = Path(argv[1]) if len(argv) > 1 else Path('tests')
    src_dir = Path(argv[2]) if len(argv) > 2 else Path('src')
    debt_path = Path(argv[3]) if len(argv) > 3 else tests_dir / 'known-debt-grep-patterns.txt'

    patterns = extract_patterns(tests_dir)
    if not patterns:
        print('found zero test patterns -- the scan is broken, not the tests', file=sys.stderr)
        return 1
    producers = extract_producers(src_dir) + extract_code_tokens(src_dir)
    if not producers:
        print('found zero producer format strings -- the scan is broken, not the code',
              file=sys.stderr)
        return 1
    producer_wordsets = build_producer_wordsets(producers)

    debt = load_debt(debt_path)
    unmapped = []
    mapped = 0
    debted = 0
    self_ref = 0

    for f, l, pat in patterns:
        if SELF_REFERENTIAL.search(pat):
            self_ref += 1
            continue
        pw = words(pat)
        if matches(pw, producer_wordsets):
            mapped += 1
            continue
        if (f, l) in debt:
            debted += 1
            continue
        unmapped.append((f, l, pat))

    if unmapped:
        print(f'{len(unmapped)} test pattern(s) match no producer and are not in '
              f'{debt_path.name}:', file=sys.stderr)
        for f, l, pat in unmapped[:40]:
            print(f'  {f}:{l}: {pat!r}', file=sys.stderr)
        if len(unmapped) > 40:
            print(f'  ... and {len(unmapped) - 40} more', file=sys.stderr)
        return 1

    if len(debt) > DEBT_FLOOR:
        print(f'{debt_path.name} carries {len(debt)} entries, over the floor of '
              f'{DEBT_FLOOR} -- the debt list may only shrink', file=sys.stderr)
        return 1

    print(f'OK: {len(patterns)} test patterns checked -- {mapped} matched a producer, '
          f'{debted} tracked as debt (floor {DEBT_FLOOR}), {self_ref} self-referential '
          f'(harness transcript or source shape, not a daemon line), 0 unmapped.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
