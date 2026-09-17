#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, issue #102: ONE PARENT'S TAGGED TRAFFIC IS NOT ANOTHER PARENT'S VERDICT.
#
# Measured on the rig 2026-09-14, reac-pw 1.0.5. The USB NIC `enp128s20f0u2` was on a
# DIRECT cable to a cold S-4000S-3208 and tcpdump on it saw no tagged frame — no frame at
# all — over 25 s. Every daemon start logged `[enp128s20f0u2] tagged REAC heard — vid 11
# (1 frame(s))` and the same for vid 12: exactly ONE frame per VLAN the daemon itself
# masters on ANOTHER parent (enp131s0.11/.12/.13). The direct link was then refused as a
# trunk for ever and the cold box got no master.
#
# THE MECHANISM, AND WHAT CHANGED (src/main.c, on_topo_io's header). Up to libreac 1.2.1
# the tap was socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) bound to the parent's ifindex a
# few syscalls later, and a packet socket created with a non-zero protocol is live on EVERY
# interface until bind(), so whatever 0x8819 the machine carried in that window was queued
# and read back as this parent's. libreac 1.2.2 creates the socket deaf (protocol 0) and
# gives ETH_P_ALL to the bind (libreac #18), and meson.build's floor now demands it. This
# test does not move: it is the DAEMON's arm of the same job, it passes on either library,
# and it is what would go red if a future tap — or a future reader of it — let a foreign
# frame decide a parent again. PACKET_IGNORE_OUTGOING never covered any of it: it is set
# after the open, it drops frames as they arrive rather than the queue, and it says nothing
# about another interface's INBOUND traffic.
#
# THE RIG'S OWN CONDITION, reproduced, in BOTH the flavours a wide-open tap queues:
#   - `noise0.11/.12/.13` carry tagged REAC at 4000 frames a second each, transmitted from
#     THIS namespace. That is the rig exactly — the daemon's own VLAN masters on
#     enp131s0 — and it reaches every unbound packet socket as PACKET_OUTGOING on
#     noise0's ifindex. `noise0`'s own peer lives in a nested namespace, so the daemon
#     never hears these frames arrive anywhere and nothing about them is evidence.
#   - `taint0` receives tagged REAC from a box on M in the nested namespace, so it is a
#     real trunk with real INBOUND tags — the other half of what an unbound tap queues,
#     and the presence control below.
#   - `direct0` is the cold box's cable: carrier, no REAC at all, pinned master.
#
# MEASURED WHILE WRITING THIS. A probe that mimics reac_topo_tap_open and counts what its
# queue already holds at bind(): 6 to 16 foreign frames on 7 of 8 opens. Against the 1.0.5
# daemon in this fixture: 7 tap opens, 7 "this parent carries tagged REAC" sentences on a
# cable with nothing on it. The rig needed one frame.
#
# PRESENCE BEFORE ABSENCE: this test asserts `taint0` IS heard as a trunk before it asserts
# `direct0` is not. Without that, a daemon that classified nothing at all would pass.
#
# The link on `direct0` is bounced four times, because a bounce re-opens the tap and every
# open is another unbound window — the rig saw ~10 frames per VLAN across one bounce. EACH
# DOWN LASTS LONGER THAN THE IFSCAN HOLD, and that is not a detail: a flap shorter than the
# hold is kept on purpose ("link back inside the hold — segment kept"), the tap is never
# closed, and twelve such flaps produced exactly one tap open and no taint at all. A bounce
# that does not re-open the tap measures nothing.
#
# Isolation is part of the test (tests/hearing-finds-a-segment.sh's header has the
# reasoning). Skips (77) where the namespaces, iproute2 or PipeWire are unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter  >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v ip       >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-cli   >/dev/null 2>&1 || { echo "SKIP: no pw-cli"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
BIN="$1"; FAKE="$2"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)

export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

wait_for() {   # wait_for <pattern> <secs>
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -q "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

unshare -n sleep 900 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }
in_peer="nsenter -t $NSPID -n"

# THREE INDEPENDENT veth pairs: nothing on one can reach another, which is the whole point
# — the kernel never delivers noise0's or taint0's frames to direct0, and tcpdump on the
# rig's USB NIC said exactly that about the cable in the report.
ip link add noise0  type veth peer name npeer0 || exit 90
ip link add taint0  type veth peer name tpeer0 || exit 90
ip link add direct0 type veth peer name dpeer0 || exit 90
for p in npeer0 tpeer0 dpeer0; do ip link set $p netns $NSPID || exit 90; done
# OUR OWN TAGGED EGRESS, on a parent the daemon can see but hears nothing on (its peer is
# in the other namespace, so these frames arrive nowhere here). VIDs 11/12/13 are the rig's.
for v in 11 12 13; do
	ip link add link noise0 name noise0.$v type vlan id $v || exit 90
done
# THE GENUINE TRUNK, on vid 21 so a tag misattributed to direct0 names its real source.
peer ip link add link tpeer0 name tpeer0.21 type vlan id 21 || exit 90

# THE COLD BOX'S CABLE IS PINNED MASTER — the issue's own configuration, and the thing the
# stale verdict refused.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment direct0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"

ip link set noise0 up;  peer ip link set npeer0 up
ip link set taint0 up;  peer ip link set tpeer0 up
ip link set direct0 up; peer ip link set dpeer0 up
for v in 11 12 13; do ip link set noise0.$v up; done
peer ip link set tpeer0.21 up

# Three tagged sources (the rig's three VLAN masters, its own count) transmitted from THIS
# namespace, at the top rate the emulator paces; and one real box on M behind the trunk.
# All four run BEFORE the daemon, so its very first tap open happens under the traffic.
for v in 11 12 13; do
	"$FAKE" noise0.$v 00:40:ab:c4:$v:21 8 8000 >"$RT/noise$v.log" 2>&1 &
done
$in_peer "$FAKE" tpeer0.21 00:40:ab:c4:21:21 8 2000 >"$RT/tag21.log" 2>&1 &
sleep 1

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- 1. PRESENCE. The tagged parent IS a trunk. Without this the absence below is the
# absence of a working classifier, not the absence of a defect.
wait_for "\[taint0\] tagged REAC heard — vid 21" 30 || {
	echo "FAIL: tagged REAC was never heard on the parent that carries it — this run"
	echo "      cannot say anything about attribution"; tail -25 "$LOG"
	tail -3 "$RT/tag21.log"; exit 1; }
wait_for "\[taint0\] this parent carries tagged REAC, so it is not itself a segment" 20 || {
	echo "FAIL: the tag was heard and the parent was never classified a trunk"
	tail -25 "$LOG"; exit 1; }

# ---- 2. THE BOUNCES. Every link-up re-opens direct0's tap, and every open is another
# window in which an unbound socket queues somebody else's frames.
for i in 1 2 3 4; do
	ip link set direct0 down; peer ip link set dpeer0 down
	sleep 5                      # past the ifscan hold: anything shorter keeps the tap
	peer ip link set dpeer0 up; ip link set direct0 up
	sleep 3
done
sleep 3

# ---- 3. THE VERDICT ON THE DIRECT CABLE IS ITS OWN. No tagged frame ever arrived on it,
# so it is not a trunk, nothing on it is refused for being one, and the pin is obeyed.
if grep -q "\[direct0\] tagged REAC heard" "$LOG"; then
	echo "FAIL (#102): a parent with no tagged frame on its wire was told it heard one —"
	echo "      another interface's traffic was counted against it"
	grep "direct0" "$LOG" | head -20; exit 1
fi
if grep -q "\[direct0\] this parent carries tagged REAC" "$LOG"; then
	echo "FAIL (#102): the direct cable was classified a TRUNK on another parent's frames"
	grep "direct0" "$LOG" | head -20; exit 1
fi
if grep -q "\[direct0\] untagged REAC on a trunk's native VLAN is not served" "$LOG"; then
	echo "FAIL (#102): the direct cable was refused as a trunk — this is the rig report"
	grep "direct0" "$LOG" | head -20; exit 1
fi
wait_for "\[direct0\] segment up (master, pinned by reac-pw.conf)" 40 || {
	echo "FAIL: the direct cable was never driven by its pinned master"
	grep "direct0" "$LOG" | tail -20; exit 1; }

echo "OK: a parent's trunk verdict counts only frames that arrived on it (#102)"
INNER
)
rc=$?
echo "$OUT"
exit $rc
