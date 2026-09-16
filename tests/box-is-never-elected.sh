#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `auto` NEVER ELECTS BOX (2026-09-17 spec §7). A mixer never wants a surprise stagebox
# appearing on its fabric and taking channels, so `box` — like `tap` — is reachable only
# because an operator wrote it in a file.
#
# A SOURCE-SHAPE TEST, and it has to be: the election is main.c's own switch over
# reac_hunt's verdicts, and no unit test can see that a FUTURE arm of it assigns the box
# intent. What can be seen is that nothing assigns that intent at all — every `box` in this
# daemon arrives from reac_segconf_role, which gets it from reac_role_intent_parse, which
# needs the literal word in a file.
#
# EVERY ABSENCE HERE IS PROVEN AGAINST A PRESENCE FIRST (the empty-grep trap): the same
# pattern is required to MATCH for the intents that ARE elected, so a broken grep cannot
# read as a clean daemon.
set -o pipefail
cd "$(dirname "$0")/.." || exit 2
SRC=src/main.c
[ -f "$SRC" ] || { echo "box-is-never-elected: no $SRC"; exit 2; }

fail=0

# The positive control: the vocabulary IS used here, so a pattern that finds nothing below
# is finding nothing about a file it can actually read.
if ! grep -q "REAC_ROLE_INTENT_BOX" "$SRC"; then
	echo "FAIL: main.c never mentions REAC_ROLE_INTENT_BOX — this test is looking at the wrong file"
	fail=1
fi
# And the control for the ASSIGNMENT pattern itself: master/slave/auto ARE assigned.
if ! grep -qE "=[[:space:]]*REAC_ROLE_INTENT_(AUTO|MASTER|SLAVE)" "$SRC"; then
	echo "FAIL: the assignment pattern matches nothing at all — it cannot prove an absence"
	fail=1
fi

# The claim. An assignment of the box intent is an ELECTION arriving at it; the conf path
# assigns the PARSED value (`c->role_intent = i`), never this constant.
hits=$(grep -nE "=[[:space:]]*REAC_ROLE_INTENT_BOX" "$SRC" | grep -v "==")
if [ -n "$hits" ]; then
	echo "FAIL: something in main.c ELECTS the box intent:"
	echo "$hits"
	fail=1
fi

# The hunt's own verdict vocabulary has no box in it either — the other half of the same
# claim, read from the header a future arm would have to widen first.
HUNT=$(pkg-config --variable=includedir libreac 2>/dev/null)/reac/reac_hunt.h
[ -f "$HUNT" ] || HUNT=../libreac/include/reac/reac_hunt.h
if [ -f "$HUNT" ]; then
	grep -q "REAC_HUNT_MASTER" "$HUNT" || { echo "FAIL: cannot read the hunt's verdicts from $HUNT"; fail=1; }
	if grep -qiE "REAC_HUNT_BOX\b" "$HUNT"; then
		echo "FAIL: the hunt has grown a BOX verdict — an election can now arrive at it"
		fail=1
	fi
else
	echo "note: reac_hunt.h not found; the verdict half of this test did not run"
fi

[ $fail -eq 0 ] && echo "OK: nothing elects the box intent — it is reachable only from reac-pw.conf, and the hunt has no box verdict"
exit $fail
