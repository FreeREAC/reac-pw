#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# A WHOLE-BINARY TEST'S rc IS A VERDICT, NOT AN EXCUSE (audit 2026-09-24, H3).
#
# The namespace tests run their body in `OUT=$(unshare ... <<'INNER')` and read its rc. For
# months, seventeen of them turned ANY non-zero rc into SKIP. The body exits 91 when the
# daemon dies at start, so a crash in listener_open read as SKIP, before the script reached
# its own `daemon-died && fail` line, and the suite stayed green. The rule since:
#
#   - a `daemon-died` marker is a FAIL whatever rc came with it;
#   - rc 77 is the only SKIP;
#   - every other non-zero rc is a FAIL.
#
# THIS IS THE RATCHET. It looks for the shapes that turned an rc into a SKIP:
#   [ $rc -eq 0 ] || { echo "SKIP: ...    (any non-zero rc skips, on one line or two)
#   [ $rc -ne 0 ] && ... exit $SKIP       (the same, written the other way)
#   case "$OUT" in *SKIP:*) exit $SKIP    (a SKIP: anywhere in the output skips)
#
# POSITIVE CONTROL FIRST: the pattern must catch a line of each shape before a clean scan of
# tests/ counts as clean.
set -u
cd "$(dirname "$0")/.." || exit 1

BAD='\[ *"?\$[a-z]*rc"? +-(eq +0 *\] *\|\||ne +0 *\] *&&).*(exit (\$SKIP|77)|echo "SKIP)|^[[:space:]]*\*"?SKIP:.*\) *exit (\$SKIP|77)'

ctl=0
for line in \
	'[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"; exit $SKIP; }' \
	'[ $mrc -eq 0 ] || { echo "SKIP: the master end could not run (rc=$mrc)"; exit 77; }' \
	'[ $rc -ne 0 ] && exit $SKIP' \
	'[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"' \
	'	*SKIP:*) exit $SKIP ;;'; do
	printf '%s\n' "$line" | grep -qE "$BAD" && ctl=$((ctl + 1))
done
[ "$ctl" -eq 5 ] || {
	echo "FAIL: the pattern caught $ctl of 5 control lines — this scan is broken, and a broken"
	echo "      scan reports a clean tree exactly the way a clean tree does."; exit 1; }

scanned=0
for f in tests/*.sh; do
	[ "$f" = "tests/netns-rc-is-a-verdict.sh" ] && continue
	scanned=$((scanned + 1))
done
[ "$scanned" -ge 10 ] || { echo "FAIL: only $scanned test script(s) found — the scan is broken"; exit 1; }

hits=$(grep -nE "$BAD" tests/*.sh | grep -v '^tests/netns-rc-is-a-verdict.sh:')
[ -z "$hits" ] || {
	echo "FAIL: a test turns an rc into a SKIP. Skip on rc 77 only, FAIL on a daemon-died marker"
	echo "      first, and FAIL on every other non-zero rc:"
	echo "$hits" | sed 's/^/  /'
	exit 1; }

echo "OK: $scanned test scripts scanned; no rc other than 77 becomes a SKIP (control: 5/5 caught)"
exit 0
