#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: SEVERAL --live SEGMENTS ON ONE PORT SHARE ITS LINK BUDGET (audit 2026-09-24,
# M8).
#
# The link-budget admission and the wake ladder's sibling guard walk g_hear's listener
# table, which only hearing_start ever set. Under `--live a,a.11,a.12` that table was empty,
# so every master was admitted onto one port whatever it cost -- the overcommit the budget
# exists to refuse (a qdisc that then drops frames from every segment on the port).
#
# A dummy port and two VLANs on it, capped at 100 Mbit (`--set REACPW_LINK_MBIT=100`, the
# shipped knob); three masters at the default rate do not fit. No PipeWire is needed: the
# admission runs in listener_open, before any node. The CONTROL is the first segment: it
# must come up as a master, or no refusal below is about the budget.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
BIN=$(readlink -f "$BIN")
SKIP=77

for t in unshare ip timeout; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n sh -c 'ip link add lb0 type dummy && ip link add link lb0 name lb0.11 type vlan id 11' \
	2>/dev/null || { echo "SKIP: no dummy+VLAN link in a user+net namespace"; exit $SKIP; }

D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
unshare -r -n bash -c '
	ip link add lb0 type dummy &&
	ip link add link lb0 name lb0.11 type vlan id 11 &&
	ip link add link lb0 name lb0.12 type vlan id 12 || exit 90
	for i in lb0 lb0.11 lb0.12; do ip link set "$i" up; done
	HOME="$0" timeout 5 "$1" --live lb0,lb0.11,lb0.12 --tx lb0 --set REACPW_LINK_MBIT=100' \
	"$D" "$BIN" >"$D/log" 2>&1
grep -a "MASTER role\|E_LINK_BUDGET\|LINK_MBIT" "$D/log" | sed 's/^/  /'

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
grep -qa "S_KNOB_SET knob REACPW_LINK_MBIT=100" "$D/log" \
	|| fail "the port was never capped at 100 Mbit, so nothing below is a budget measurement"
grep -qa "\[lb0\] MASTER role" "$D/log" \
	|| fail "the first segment did not come up as a master -- the control failed, so a refusal below says nothing"
N=$(grep -ac "E_LINK_BUDGET" "$D/log")
[ "$N" -ge 1 ] \
	|| fail "three --live masters were all admitted onto one 100 Mbit port: the budget saw an empty table (M8)"
[ $FAIL -eq 0 ] || exit 1
echo "OK: --live segments on one port are admitted against one budget ($N refused)"
