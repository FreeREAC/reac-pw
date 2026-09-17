#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# EVERY NAMESPACE TEST IS IN THE `netns` SUITE, AND THE RPM RUNS THAT SUITE ONE AT A TIME.
#
# A test that mints a veth pair, a private PipeWire and a nested network namespace is not
# parallel-safe with another one doing the same: they contend for the machine's namespace
# and PipeWire startup budget, and the loser times out. That is the whole of the packaging
# flake — `meson test` defaults to one process per core, so an rpmbuild on a loaded builder
# ran a dozen of them at once and blamed the daemon.
#
# THE SUITE IS THE MECHANISM AND THIS IS ITS RATCHET: a new namespace test that forgets
# `suite : 'netns'` would rejoin the parallel pool silently, which is exactly the flake
# coming back. The scan is keyed on what a script DOES -- it invokes the namespace tool
# with its flags -- never on a hand-kept list. The pattern carries a flag so that prose
# ABOUT the tool (this comment included) is not read as a test that mints one.
#
# POSITIVE CONTROL FIRST: an empty scan and a fully tagged tree look identical, so this
# refuses to pass until it has found namespace tests to check.
set -u
cd "$(dirname "$0")/.." || exit 1
MB=meson.build
SPEC=packaging/reac-pw.spec
[ -f "$MB" ] && [ -f "$SPEC" ] || { echo "FAIL: run from the reac-pw tree ($MB / $SPEC)"; exit 1; }

FAIL=0
found=0 tagged=0
for f in tests/*.sh; do
	# THIS FILE NAMES THE PATTERN, IT DOES NOT MINT A NAMESPACE -- the one exemption, and
	# it is by path so no other script can claim it.
	[ "$f" = "tests/netns-tests-are-serial.sh" ] && continue
	grep -q 'unshare -' "$f" || continue
	found=$((found + 1))
	# The declaration block: the find_program line and the four lines under it.
	if grep -A 4 "find_program('$f')" "$MB" | grep -qE "suite : ('netns'|\[.*'netns'.*\])"; then
		tagged=$((tagged + 1))
	else
		echo "FAIL: $f mints namespaces but is not declared in the 'netns' suite —"
		echo "      add \`suite : 'netns',\` to its test() in $MB, or it runs in parallel"
		echo "      with the other namespace tests and flakes the rpm %check."
		FAIL=1
	fi
done

# The control: the scan must have found the namespace tests. A zero here means the scan
# broke, not that the tree is clean -- the two read identically.
[ "$found" -ge 10 ] || {
	echo "FAIL: only $found namespace test(s) found in tests/ — this scan is broken, and a"
	echo "      broken scan reports a clean tree exactly the way a clean tree does."
	FAIL=1; }

# AND THE PACKAGE HAS TO ACTUALLY RUN IT SERIALLY. A suite nothing passes --num-processes 1
# to is a label.
grep -q -- '--suite netns --num-processes 1' "$SPEC" || {
	echo "FAIL: $SPEC %check does not run the netns suite with --num-processes 1"; FAIL=1; }
grep -q -- '--no-suite netns' "$SPEC" || {
	echo "FAIL: $SPEC %check does not exclude the netns suite from its parallel run —"
	echo "      those tests would run twice, the second time in parallel."; FAIL=1; }

[ "$FAIL" = 0 ] && echo "OK: $tagged/$found namespace tests are in the netns suite, and the rpm %check runs that suite serially"
exit $FAIL
