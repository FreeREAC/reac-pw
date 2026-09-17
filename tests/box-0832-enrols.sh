#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, END TO END: AN 8-IN / 32-OUT SPLIT BOX ENROLS AT ITS OWN WIDTH.
#
# THE CHASSIS IS LABELLED S-4000H AND THE WIRE CALLS IT AN S-4000S. A real M-200 driving
# this very box displays "S-4000S, 08 in / 32 out" because the box sends no name record and
# the 0x84 selector's default label is S-4000S. The table row and this test use the wire's
# name, `s4000s-0832`.
#
# THE JOB, AND THE MORNING IT WAS NOT DONE. 2026-09-17 07:00, VLAN 13, reac-pw 1.0.14: the
# operator plugged in an S-4000H-0832 and the roster read `state=probing model=none
# role=master width=0/0` for minutes, twice over the same MAC:
#
#     REAC heard — box 00:40:ab:c4:25:80 (8 ch): this interface is a segment
#     REAC heard — unknown 00:40:ab:c4:25:80 (32 ch): this interface is a segment
#
# The unit proof is libreac's tests/test_box_0832.c, which replays that box's own captured
# frames through the decoder, the table, the classifier and the master FSM. It cannot see
# what this sees: the real binary, on a real wire, publishing a real width.
#
# THE FAR END IS libreac's fake_box UNDER A MODEL TOKEN — `fake_box <if> <secs>
# s4000s-0832` — so it declares the row captured from that very box (input groups marked
# 0x00, outputs written first) and returns the 8 channels a real M-200 measured it
# returning once granted (217 905 frames of 340 B, m200-s4000h-coldboot.pcap).
#
# THE SECOND ARM IS THE CONTROL, and the first proves nothing without it: the SAME binary
# and the SAME wire with `s4000s` must publish 32x8. A probe that reports 8x32 from a
# constant would pass arm 1 alone; one that cannot tell two chassis apart cannot testify
# about either.
#
# ISOLATION: as tests/box-wakes-on-a-phy-edge.sh — an unprivileged user+net+pid namespace
# with its own PipeWire on a private runtime dir, the peer end of the veth in a NESTED
# network namespace. Nothing here touches the live graph.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
FAKE="${2:-}"
SKIP=77

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

SECS="${REACPW_0832_SECS:-30}"

run_arm() {   # run_arm <model-token>
	unshare -r -n -m -p -f --mount-proc --map-root-user \
		bash -s -- "$BIN" "$FAKE" "$SECS" "$1" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; SECS="$3"; MODEL="$4"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"

ip link add spl0 type veth peer name splb0 || exit 90
ip link set splb0 netns $NSPID || exit 90
ip link set spl0 up; $in_peer ip link set splb0 up

$in_peer "$FAKE" splb0 "$SECS" "$MODEL" >"$RT/box.log" 2>&1 &
FAKEPID=$!

HOME="$CONF" REAC_DEBUG=1 "$BIN" --live spl0 --tx spl0 --mixer m5000 --rate 96000 \
	--name split >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || { echo "daemon exited early"; tail -5 "$LOG"; exit 91; }

for ((i = 0; i < SECS * 2; i++)); do
	grep -q "ESTABLISHED" "$LOG" && break
	sleep 0.5
done
sleep 2
# THE WIDTH IS READ FROM THE GRAPH, not from the log: this is what a console sees.
pw-cli ls Node 2>/dev/null | grep -A 40 "reac" > "$RT/nodes.txt" || true
pw-dump 2>/dev/null | grep -E "reac.box.model|reac.box.width|reac.link-state" \
	| tr -d '",' | sort -u > "$RT/props.txt" || true
kill -TERM $PID 2>/dev/null; sleep 0.5
wait $FAKEPID; BOXRC=$?

echo "--- box ---"; cat "$RT/box.log"
echo "--- props ---"; cat "$RT/props.txt"
echo "--- daemon ---"; grep -E "recognized box|box declared|PROBING|GRANTING|ESTABLISHED|REAC heard|COULD NOT|REFUSED" "$LOG" | tail -25
echo "BOXRC=$BOXRC"
INNER
}

FAIL=0
say() { printf '%s\n' "$*"; }

# ---- ARM 1: the split box. 8 inputs declared, 32 channels returned, and the daemon has
# to publish the DECLARATION's geometry rather than the frame's.
A1=$(run_arm s4000s-0832 2>&1) || true
case "$A1" in *"SKIP: "*) echo "${A1##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
say "$A1"
echo "$A1" | grep -q "BOXRC=0" || { say "FAIL: the 0832 split never enrolled (fake_box exit != 0)"; FAIL=1; }
echo "$A1" | grep -q "ESTABLISHED" || { say "FAIL: the master never reached ESTABLISHED"; FAIL=1; }
echo "$A1" | grep -q "recognized box = S-4000S-0832" || { say "FAIL: the box was not NAMED from its declaration"; FAIL=1; }
echo "$A1" | grep -q "reac.box.width.*8x32" || { say "FAIL: the published width is not 8x32 — the declaration did not reach the graph"; FAIL=1; }
# ONE MAC, ONE VERDICT: the sniffer's line may appear, but never as `unknown` for a box
# that has declared itself.
echo "$A1" | grep -E "REAC heard — unknown" >/dev/null && { say "FAIL: the same box was also reported as an unknown peer"; FAIL=1; }

# ---- ARM 2: the control. The same everything at 32/8 — a probe that cannot tell the two
# apart cannot testify about either.
A2=$(run_arm s4000s 2>&1) || true
case "$A2" in *"SKIP: "*) say "SKIP: the control arm could not run"; exit $SKIP;; esac
say "$A2"
echo "$A2" | grep -q "BOXRC=0" || { say "FAIL (control): the S-4000S did not enrol"; FAIL=1; }
echo "$A2" | grep -q "reac.box.width.*32x8" || { say "FAIL (control): the S-4000S did not publish 32x8 — this harness cannot tell two chassis apart"; FAIL=1; }

[ "$FAIL" = 0 ] && say "PASS: an S-4000H-0832 enrols at its DECLARED 8x32 while returning 32 channels, and the same harness reads 32x8 for an S-4000S"
exit $FAIL
