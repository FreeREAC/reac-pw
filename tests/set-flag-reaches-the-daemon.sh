#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# `--set KEY=VALUE` ACTUALLY WORKS ON A COMMAND LINE.
#
# The operator's 2026-09-17 ruling gave the daemon a generic `--set KEY=VALUE` at the top
# of the knob precedence, --help documents it, and test_reac_knob_table.c proves the
# resolver behind it — cli beats env, the last --set wins, an unknown key is refused. What
# nothing proved is that the FLAG reaches the daemon: `--set` was read by a pre-pass over
# argv and then fell through the main flag loop's final `else` into `usage()`, so every
# real invocation carrying one exited 2 before opening anything. Found 2026-09-20, by the
# first test that ever passed `--set` to the binary (tests/empty-master-yields-the-budget.sh,
# which needs REACPW_LINK_MBIT). A unit test over a function the product path never calls
# is the oldest false signal in this project.
#
# THE NAMESPACE IS NOT THE POINT HERE, it is the cheapest way to hold CAP_NET_RAW: the
# capability preflight runs BEFORE the flag loop, so outside a namespace the daemon refuses
# to start for a different reason and this question cannot be asked at all. Nothing is
# minted, no netdev, no PipeWire, no frame — an empty network namespace and one process
# that exits.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
BIN=$(readlink -f "$BIN")
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
unshare -r -n true 2>/dev/null || { echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }

# ---- 1. THE CONTROL, and it comes first: this probe has to be able to SEE the defect it
#         is named for. An unknown flag must produce the usage text and exit 2 — which is
#         exactly what `--set` produced for three days.
OUT=$(unshare -r -n env HOME=/nonexistent "$BIN" --nope 2>&1); rc=$?
[ "$rc" = "2" ] || fail "an unknown flag did not exit 2 (got $rc) — this probe cannot tell a refused flag from an accepted one, so nothing below is a measurement"
echo "$OUT" | grep -q "^usage:" \
	|| fail "an unknown flag printed no usage text — the probe's discriminator does not discriminate"

# ---- 2. A KNOWN KNOB IS ACCEPTED BY THE FLAG LOOP and announced at the cli layer. The
#         daemon is killed after a few seconds: what is asserted is that it got PAST
#         argument parsing, not what it did next.
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
unshare -r -n env HOME="$D" timeout 5 "$BIN" --set REAC_DEBUG=1 --pcap /nonexistent.pcap \
	>"$D/set.log" 2>&1
if grep -q "^usage:" "$D/set.log"; then
	fail "--set REAC_DEBUG=1 was refused by the flag loop — the flag --help documents does not reach the daemon (#107 lane, 2026-09-20)"
	head -3 "$D/set.log" | sed 's/^/  /'
fi
grep -qa "S_KNOB_SET knob REAC_DEBUG=1 (cli)" "$D/set.log" \
	|| { fail "--set did not announce its knob at the cli layer"; head -5 "$D/set.log" | sed 's/^/  /'; }

# ---- 3. AND AN UNKNOWN KNOB IS STILL REFUSED, with its code, before anything opens.
OUT=$(unshare -r -n env HOME=/nonexistent "$BIN" --set NOPE=1 --pcap /dev/null 2>&1); rc=$?
[ "$rc" = "2" ] || fail "--set with an unknown key did not exit 2 (got $rc)"
echo "$OUT" | grep -q "E_UNKNOWN_KNOB" \
	|| fail "--set with an unknown key did not refuse by code"

[ $FAIL -eq 0 ] || exit 1
echo "PASS: --set KEY=VALUE reaches the daemon, announces at the cli layer, and still refuses an unknown key"
