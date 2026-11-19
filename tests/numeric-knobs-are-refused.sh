#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# A NUMERIC KNOB THAT IS NOT A NUMBER IN RANGE IS REFUSED AT START, BY NAME.
#
# The defect: REACPW_GRANT_DWELL_MS/_S, REACPW_CATCHUP_MAX_SLOTS,
# REACPW_BOX_MASTER_PRESILENCE_MS and REACPW_LINK_MBIT went through strtol/atoi with
# no end pointer and no range check. "5s" became 5, "-1" became -1, became 0, and
# the daemon started on a value the operator never wrote and never heard about.
#
# Reached by every layer that can carry a knob: the environment (every case below), the
# command line (`--set KEY=VALUE`) and the conf file (~/.config/reac-pw/reac-pw.env).
#
# Asserted on the whole binary and BEFORE any capability is needed: the door that
# refuses a bad number sits ahead of the root refusal and the capability preflight, so
# this runs on any host. Every bad value must exit 2 and name the key and the value on
# stderr; a good value of the same key must not draw that refusal.
set -u
BIN="${1:?usage: numeric-knobs-are-refused.sh /path/to/reac-pw}"

fail=0
bad() {   # key value
	local out rc
	out=$(env HOME=/nonexistent-reac-pw-test "$1=$2" timeout 10 "$BIN" 2>&1); rc=$?
	if [ "$rc" -ne 2 ] || ! grep -qF -- "$1='$2'" <<<"$out"; then
		echo "FAIL: $1='$2' was not refused by name (rc=$rc)"
		echo "$out" | head -4 | sed 's/^/      /'
		fail=1
	fi
}
bad_set() {   # key value: the same refusal from the command line, naming the layer
	local out rc
	out=$(env HOME=/nonexistent-reac-pw-test timeout 10 "$BIN" --set "$1=$2" 2>&1); rc=$?
	if [ "$rc" -ne 2 ] || ! grep -qF -- "$1='$2' from the command line refused" <<<"$out"; then
		echo "FAIL: --set $1=$2 was not refused by name from the command line (rc=$rc)"
		echo "$out" | head -4 | sed 's/^/      /'
		fail=1
	fi
}
bad_conf() {  # key value: the same refusal from the conf file
	local out rc h
	h=$(mktemp -d); mkdir -p "$h/.config/reac-pw"
	printf '%s=%s\n' "$1" "$2" > "$h/.config/reac-pw/reac-pw.env"
	out=$(env HOME="$h" timeout 10 "$BIN" 2>&1); rc=$?
	rm -rf "$h"
	if [ "$rc" -ne 2 ] || ! grep -qF -- "$1='$2' from ~/.config/reac-pw/reac-pw.env (per-host) refused" <<<"$out"; then
		echo "FAIL: $1=$2 in the conf file was not refused by name from the conf file (rc=$rc)"
		echo "$out" | head -4 | sed 's/^/      /'
		fail=1
	fi
}
good() {  # key value
	local out
	out=$(env HOME=/nonexistent-reac-pw-test "$1=$2" timeout 10 "$BIN" 2>&1)
	if grep -qF -- "$1='$2'" <<<"$out" && grep -qi "refus" <<<"$(grep -F -- "$1" <<<"$out")"; then
		echo "FAIL: $1='$2' is in range and was refused"
		echo "$out" | head -4 | sed 's/^/      /'
		fail=1
	fi
}

for v in 5s -1 " 5" 0x10 99999999999999999999 600000 1.5; do bad REACPW_GRANT_DWELL_MS "$v"; done
for v in 5s -1 3601 99999999999999999999;                do bad REACPW_GRANT_DWELL_S "$v"; done
for v in 5s -2 1.5 99999999999999999999 100001;          do bad REACPW_CATCHUP_MAX_SLOTS "$v"; done
for v in 5s -1 600001;                                   do bad REACPW_BOX_MASTER_PRESILENCE_MS "$v"; done
for v in 100x -1 0 1000001;                              do bad REACPW_LINK_MBIT "$v"; done
for v in 512x 95 4097 -1 99999999999999999999;           do bad REACPW_GUARD_FLOOR_FRAMES "$v"; done

bad_set  REACPW_GRANT_DWELL_MS 5s
bad_conf REACPW_CATCHUP_MAX_SLOTS 5s

good REACPW_GRANT_DWELL_MS 5000
good REACPW_GRANT_DWELL_S 5
good REACPW_CATCHUP_MAX_SLOTS -1
good REACPW_CATCHUP_MAX_SLOTS 17
good REACPW_BOX_MASTER_PRESILENCE_MS 4000
good REACPW_LINK_MBIT 1000
good REACPW_GUARD_FLOOR_FRAMES 96
good REACPW_GUARD_FLOOR_FRAMES 4096

[ "$fail" -eq 0 ] && echo "PASS: numeric knobs are refused by name at start"
exit "$fail"
