#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# A REBUILT reac-playback PUBLISHES EVERY PROP AGAIN (audit 2026-09-24, M4).
#
# Each sink_publish_* compares the live answer against a `*_last` shadow and skips the
# update when they agree. A rebuilt node is a NEW pw_stream with only its seed props, so
# a shadow left at the old node's answer suppresses the first publish onto the new one:
# every box re-enrolment left reac.rate* and reac.cfg.role.* off the playback node,
# because sink_open_filter re-seeded every shadow but those two families.
#
# The check: every `*_last` field of struct reac_sink_node is assigned in
# sink_seed_publish_shadows, and sink_open_filter calls it. A new shadow that is not
# re-seeded there fails here instead of on the desk. no-box-no-node.sh's return arm is
# the whole-binary half (it needs namespaces).
#
# Run directly: `python3 tests/sink-shadows-reset-on-rebuild.py [src/reac_sink_node.c]`.

import re
import sys
from pathlib import Path

src = Path(sys.argv[1] if len(sys.argv) > 1 else
           Path(__file__).resolve().parent.parent / 'src' / 'reac_sink_node.c').read_text()


def body(head):
    """The brace-balanced body that follows the first match of `head`, or None."""
    m = re.search(head, src, re.M)
    if not m:
        return None
    i = src.index('{', m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        depth += {'{': 1, '}': -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j + 1]
    return None


fail = []
struct = body(r'^struct reac_sink_node \{')
if struct is None:
    sys.exit('FAIL: no struct reac_sink_node in the source')
shadows = sorted(set(re.findall(r'\b(\w+_last)\b(?:\[[^\]]*\])?;', struct)))
if not shadows:
    sys.exit('FAIL: struct reac_sink_node declares no *_last shadow -- the parse is broken')

seed = body(r'^static void sink_seed_publish_shadows\(struct reac_sink_node \*n\)\s*\{')
if seed is None:
    fail.append('no sink_seed_publish_shadows(): the shadows are not seeded in one place')
else:
    for f in shadows:
        if not re.search(r'n->%s\b' % f, seed):
            fail.append('%s is not re-seeded: a rebuilt node never gets its first publish' % f)

opener = body(r'^static int sink_open_filter\(struct reac_sink_node \*n, const char \*label\)\s*\{')
if opener is None:
    fail.append('no sink_open_filter() -- the parse is broken')
elif 'sink_seed_publish_shadows(n)' not in opener:
    fail.append('sink_open_filter does not call sink_seed_publish_shadows(n)')

print('shadows %d: %s' % (len(shadows), ' '.join(shadows)))
for f in fail:
    print('FAIL:', f)
if fail:
    sys.exit(1)
print('OK')
