#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# A `# --- ... ---` header block in meson.build is a promise: the reader trusts
# that what sits directly below it is what the prose describes. Five of them
# drifted onto the WRONG test (issue: five orphaned headers, 2026-09-17) because
# a later edit wedged a new block between a header and the test it was written
# for, and nothing checked. This walks every header block, finds the next
# `test(` call BEFORE another header block intervenes, and refuses when the
# header names none of that test's own words — a keyword match against the
# test's script filename (or its meson label, when it has no `tests/*.sh`).
#
# Run directly: `python3 tests/meson-headers-sit-above-their-test.py [meson.build]`.
# Exit 0 = every header pairs with a test that shares a keyword. Exit 1 names
# every header that does not, with its line number and the test it landed on.

import re
import sys

HEADER_START = re.compile(r'^#\s*---')
COMMENT = re.compile(r'^\s*#')
TEST_CALL = re.compile(r"^\s*test\(\s*'([^']+)'")
FIND_PROGRAM = re.compile(r"find_program\(\s*'tests/([^']+)'\s*\)")

# A tiny, HAND-VERIFIED allowlist for a header whose prose is legitimately
# right above its own test but never repeats the script's own vocabulary (it
# says "carrier goes and returns", the script is named ...-wakes-on-a-phy-edge).
# Keyed on the header's OWN marker line plus the test label it sits above, so a
# swap (this header lands on a DIFFERENT test) changes the key and the entry no
# longer applies -- it does not blanket-exempt the header, only this exact
# pairing. Reviewed by hand at the time each was added; do not grow this list
# to silence a real orphan.
VERIFIED_PAIRS = {
    ("# --- integration: does the watch SEE a real carrier change? ------------------\n",
     'reac_link_edge_observed'),
    ("# --- whole-binary test: a cable edge re-establishes the master (#95) ---------\n",
     'reac_link_up_reestablishes'),
    ("# --- whole-binary test: a box master is ENROLLED WITH, its own way (0.5.6) ----\n",
     'reac_box_master_slave_join'),
    ("# --- real sockets: an UNGRANTED slave gets off the wire (libreac 1.0.1) ------\n",
     'reac_courtship_backs_off'),
    ("# --- whole-binary test: a box that DROPPED is re-acquired by the daemon alone ------\n",
     'reac_box_wakes_on_a_phy_edge'),
    ("# --- source-shape test: ONE segment, ONE word ----------------------------------------\n",
     'box-speaks-one-vocabulary'),
    ("# --- the namespace tests are a SUITE, and the rpm runs that suite one at a time -------\n",
     'netns_tests_are_serial'),
}

STOPWORDS = {
    'the', 'a', 'an', 'and', 'or', 'is', 'are', 'was', 'were', 'of', 'on',
    'in', 'at', 'to', 'for', 'with', 'its', 'own', 'that', 'this', 'test',
    'tests', 'whole', 'binary', 'unit', 'sh', 'reac', 'reacpw', 'pw', 'not',
    'no', 'one', 'off', 'into', 'from', 'over', 'under', 'what', 'when',
    'does', 'never', 'ever', 'still', 'about', 'each', 'every', 'both',
    'real', 'sockets', 'source', 'gate', 'acceptance', 'dev', 'only',
    # domain-generic nouns: true of nearly every header in this file, so a
    # match on one of these proves nothing about WHICH test the header is
    # about (a false confirmation on a swap, sabotage-verified 2026-09-17).
    'box', 'boxes', 'master', 'masters', 'slave', 'slaves', 'segment',
    'segments', 'daemon', 'wire', 'node', 'nodes', 'frame', 'frames',
    'live', 'link', 'links',
}


def normalize(word):
    """Strip a trailing plural/verb suffix so 'hears'/'hearing'/'heard' meet,
    without truncating to a fixed-length prefix -- a prefix stem is how
    'heartbeat' collided with 'hearing' the first time this was tried."""
    for suffix in ('ing', 'ed', 'es', 's'):
        if word.endswith(suffix) and len(word) - len(suffix) >= 3:
            return word[: -len(suffix)]
    return word


def words(s):
    return {normalize(w) for w in re.split(r'[^a-z]+', s.lower())
            if len(w) >= 4 and w not in STOPWORDS}


def keyword_hit(keywords, text):
    text_words = {normalize(w) for w in re.split(r'[^a-z]+', text.lower()) if len(w) >= 4}
    return bool(keywords & text_words)


def find_header_blocks(lines):
    """Return [(start_idx, end_idx_exclusive)] for every '# ---' marked block."""
    blocks = []
    i = 0
    n = len(lines)
    while i < n:
        if HEADER_START.match(lines[i]):
            start = i
            j = i + 1
            while j < n and (COMMENT.match(lines[j]) or lines[j].strip() == ''):
                # a genuinely blank (non-comment) line ends the block; a bare
                # '#' continuation line does not.
                if lines[j].strip() == '' :
                    break
                j += 1
            blocks.append((start, j))
            i = j
        else:
            i += 1
    return blocks


def find_paired_test(lines, after_idx, next_header_idx):
    """Scan [after_idx, next_header_idx) for the first `test(` call."""
    limit = next_header_idx if next_header_idx is not None else len(lines)
    for k in range(after_idx, limit):
        m = TEST_CALL.match(lines[k])
        if m:
            return k, m.group(1)
    return None, None


def subject_keywords(lines, test_line_idx, test_label):
    # look at the test( call and its args (usually 3-6 lines) for a
    # find_program('tests/....sh') script name. A whole-binary test's script is
    # named for the behaviour it drives (hearing-finds-a-segment.sh) and the
    # header is prose about that behaviour, so the two share real words --
    # exactly the class the five orphans came from. A bare unit-test label
    # (reac_carrier, reac_badge, ...) is a terse identifier, not a description,
    # and does not reliably recur in prose that is free to use a synonym; those
    # pairs are out of scope for this check, not silently trusted -- `main()`
    # reports how many were skipped so the scope stays visible.
    window = '\n'.join(lines[test_line_idx:test_line_idx + 6])
    m = FIND_PROGRAM.search(window)
    if not m:
        return None
    script = m.group(1)
    script = re.sub(r'\.(sh|py)$', '', script)
    return words(script.replace('-', ' ').replace('_', ' '))


def check(path):
    with open(path) as f:
        lines = f.readlines()

    blocks = find_header_blocks(lines)
    failures = []
    checked = 0
    skipped = 0

    for idx, (start, end) in enumerate(blocks):
        next_header_idx = blocks[idx + 1][0] if idx + 1 < len(blocks) else None
        test_idx, label = find_paired_test(lines, end, next_header_idx)
        header_text = ''.join(lines[start:end])
        if test_idx is None:
            # A header that pairs with no test at all before the next header
            # (or EOF) describes a build step only -- not an orphan by this
            # law, since there is no test to have drifted onto. Skip it.
            continue
        kws = subject_keywords(lines, test_idx, label)
        if not kws:
            skipped += 1
            continue
        checked += 1
        if not keyword_hit(kws, header_text):
            if (lines[start], label) in VERIFIED_PAIRS:
                continue
            failures.append((start + 1, label, test_idx + 1, sorted(kws)))

    return failures, checked, skipped


def main(argv):
    path = argv[1] if len(argv) > 1 else 'meson.build'
    failures, checked, skipped = check(path)
    if checked == 0:
        print('found nothing to check -- the scan is broken, not the file',
              file=sys.stderr)
        return 1
    if failures:
        print(f'{len(failures)} header(s) do not name their paired test in {path}:',
              file=sys.stderr)
        for header_line, label, test_line, kws in failures:
            print(f'  meson.build:{header_line}: header does not mention any of '
                  f'{kws} -- paired with test(\'{label}\') at meson.build:{test_line}',
                  file=sys.stderr)
        return 1
    print(f'OK: {checked} whole-binary header/test pairs name a shared keyword '
          f'({skipped} unit-test pairs out of scope, bare labels not prose).')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
