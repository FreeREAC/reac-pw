#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# ONE WORD FOR ONE SEGMENT. A `role = box` segment was spelled three ways in one log —
# `BOX role` when its row was taken up, `SLAVE role (X-ch upstream return)` when its
# engine started and `segment up (slave, pinned by reac-pw.conf)` when it came up — while
# the conf key, the roster row and the console all say `box`. An operator reading one log
# cannot tell that those are one segment, and a grep written against one spelling misses
# the other two (tests/box-0832-enrols.sh asserted a fourth spelling nothing ever emitted).
#
# A SOURCE-SHAPE TEST, and it has to be: two of these lines are printed only when a real
# mixer is on the far end of a cable, and the third only when a PipeWire node cannot be
# sized — paths a unit test cannot reach and a whole-binary test reaches only by luck.
# What CAN be read is the text the daemon is built to print.
#
# EVERY ABSENCE IS PROVEN AGAINST A PRESENCE FIRST (the empty-grep trap): each pattern
# below is run in a form that MUST match before the form that must not.
set -o pipefail
cd "$(dirname "$0")/.." || exit 2
SRC=src/main.c
[ -f "$SRC" ] || { echo "box-speaks-one-vocabulary: no $SRC"; exit 2; }

fail=0

# ---- THE CONTROL: this file is the one that speaks, and it says `box`. -----------------
grep -q "REAC_ROLE_INTENT_BOX" "$SRC" || {
	echo "FAIL: main.c never mentions REAC_ROLE_INTENT_BOX — this test is reading the wrong file"
	fail=1; }
grep -q 'role = box — presenting' "$SRC" || {
	echo "FAIL: nothing announces a box row as \`role = box\` — the patterns below cannot prove an absence"
	fail=1; }

# ---- THE CLAIM: no second spelling of that segment survives in a printed line. ---------
HITS=$(grep -nE '"[^"]*BOX role' "$SRC" | grep -v '^\s*[0-9]*:\s*\*')
[ -z "$HITS" ] || { echo "FAIL: a log line still spells the box segment \`BOX role\`:"; echo "$HITS"; fail=1; }
HITS=$(grep -nE '"[^"]*SLAVE role \(' "$SRC")
[ -z "$HITS" ] || { echo "FAIL: a log line still spells a segment \`SLAVE role (…)\`:"; echo "$HITS"; fail=1; }

# The segment-up line asks the intent vocabulary for the word, exactly as it does for a
# tap — reac_role_name knows only the wire's two ends and would print `slave` for a box.
# Control first: the tap is already spelled that way, so the pattern matches something.
grep -q "reac_role_intent_name(REAC_ROLE_INTENT_TAP)" "$SRC" || {
	echo "FAIL: not even the tap is spelled from the intent vocabulary — pattern is wrong"; fail=1; }
N=$(grep -c "reac_role_intent_name(REAC_ROLE_INTENT_BOX)" "$SRC")
[ "${N:-0}" -ge 2 ] || {
	echo "FAIL: the box word is taken from the intent vocabulary $N time(s); the roster row and the segment-up line both need it"
	fail=1; }

# ---- AND THE reac-playback WIDTH IS ONE NUMBER, SPELLED ONCE ---------------------------
# The same class of defect one line down: the sizing was written out three times and the
# failure message reported a fourth, different one (`up_ch`) — a message about a size
# nobody tried, in a text that named the box-master caller only.
grep -q "const int sink_ch" "$SRC" || {
	echo "FAIL: the reac-playback width is not held in one variable (sink_ch)"; fail=1; }
N=$(grep -c "sink_ch" "$SRC")
[ "${N:-0}" -ge 4 ] || {
	echo "FAIL: sink_ch is used $N time(s) — the cfg, the ensure call and both failure messages need it"
	fail=1; }
HITS=$(grep -n "box master's %d outputs" "$SRC")
[ -z "$HITS" ] || { echo "FAIL: the sizing failure still names the box-master caller only:"; echo "$HITS"; fail=1; }

[ $fail -eq 0 ] && echo "OK: one segment, one word — \`box\` in the conf, the roster and every line that names it, and one spelling of the reac-playback width"
exit $fail
