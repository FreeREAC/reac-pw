#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, audit 2026-09-24 H1: AN OPEN THAT REFUSES AFTER ITS ENGINE STARTED TAKES
# THE ENGINE WITH IT.
#
# listener_open's `fail_with_nodes:` exit used to drop the nodes and nothing else, while
# its comment said the ring, the socket and the seglock "are cleaned up at each refusal
# above". Nothing above it had cleaned them. On a slave or box-master join the slave
# engine is already OPEN AND RUNNING when reac_source_node_ensure refuses, and then
# hearing_serve memsets the listener under the running thread and retries 5 s later into
# the same slot. Each retry leaked the engine's AF_PACKET socket and its rings, and the
# zeroed struct was the running thread's own state: the memset clears its `running` flag,
# which is the only reason the thread leaves at all -- unjoined, while the next open is
# already re-initialising the very struct it is still reading. Measured on main: open fds
# 26, 27, 28, 29 at the 2nd..5th refusal.
#
# THE SHAPE. A fake box master on the far end of a veth, so the hunt decides a box-master
# JOIN: the path that opens the slave engine and builds reac-playback before it builds
# reac-capture. tests/refuse_capture_shim.c (LD_PRELOAD) makes PipeWire refuse the
# `reac:capture` stream and nothing else, the way a graph that is not up yet does. The open
# refuses, hearing_serve retries every 5 s, and each retry refuses the same way.
#
# THE MEASUREMENT is the daemon's own /proc: threads and open fds, sampled as each refusal
# is logged. A refusal that releases what it opened leaves both flat from one refusal to
# the next; the leak grows both by the engine's thread and sockets every time.
#
# PRESENCE FIRST: the refusal has to be the one under test (the join reached the capture
# node and was refused there) before a flat count means anything.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master /path/to/refuse-capture-shim.so}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master /path/to/refuse-capture-shim.so}"
SHIM="${3:?usage: $0 /path/to/reac-pw /path/to/fake-box-master /path/to/refuse-capture-shim.so}"
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }
BIN=$(readlink -f "$BIN"); FAKE=$(readlink -f "$FAKE"); SHIM=$(readlink -f "$SHIM")

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" "$SHIM" <<'INNER'
set -u
BIN="$1"; FAKE="$2"; SHIM="$3"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n"

ip link add ref0 type veth peer name rbox0 || exit 90
ip link set rbox0 netns $NSPID || exit 90
$in_peer "$FAKE" rbox0 00:40:ab:c4:dc:a7 "$FACT_BOX_S0808_IN" 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
sleep 0.5
ip link set ref0 up; $in_peer ip link set rbox0 up

HOME="$CONF" LD_PRELOAD="$SHIM" "$BIN" >"$LOG" 2>&1 &
PID=$!

refusals() { grep -c "\[ref0\] heard, but the segment did not come up" "$LOG"; }
# One sample per refusal, taken as it is logged: the retry is 5 s away, so the daemon is
# between attempts and holding only what the last one left behind.
last=0
for ((i = 0; i < 60 * 5; i++)); do
	kill -0 $PID 2>/dev/null || { echo "daemon-died after $last refusal(s)"; tail -15 "$LOG"; exit 91; }
	n=$(refusals)
	if [ "$n" -gt "$last" ]; then
		sleep 0.3
		echo "sample $n threads $(ls /proc/$PID/task | wc -l) fds $(ls /proc/$PID/fd | wc -l)"
		last=$n
		[ "$n" -ge 5 ] && break
	fi
	sleep 0.2
done
echo "refusals $last"
grep -c "failed to create reac:capture node" "$LOG" | sed 's/^/capture-refused /'
grep -c "refuse-capture-shim: refused" "$LOG" | sed 's/^/shim-refused /'
grep -c "box masters this wire" "$LOG" | sed 's/^/joins /'
grep -a -E "SLAVE role on a BOX MASTER|already holds|did not come up|ORPHAN" "$LOG" | head -4 | sed 's/^/  log /'
kill -0 $PID 2>/dev/null || { echo "daemon-died at the end"; tail -15 "$LOG"; exit 91; }
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
echo "$OUT" | sed 's/^/  /'
fail() { echo "FAIL: $1"; exit 1; }
echo "$OUT" | grep -qa 'daemon-died' && fail "the daemon died while its open was being refused — the memset under a running engine is the audit's use-after-free"
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || fail "the namespace body exited rc=$rc"

val() { echo "$OUT" | awk -v k="$1" '$1 == k { print $2; exit }'; }
# 0. THE REFUSAL UNDER TEST HAPPENED, repeatedly, at the capture node of a join.
[ "$(val joins)" -ge 1 ] 2>/dev/null || fail "the daemon never joined the box master — the slave engine never started, so nothing below is about H1"
[ "$(val capture-refused)" -ge 5 ] 2>/dev/null || fail "only $(val capture-refused) capture refusal(s) were logged; the open never reached the node this test refuses"
[ "$(val refusals)" -ge 5 ] 2>/dev/null || fail "only $(val refusals) refused open(s) in 60 s; the retry never repeated enough to measure"

# 1. WHAT THE SECOND REFUSAL LEFT, THE FIFTH LEFT TOO. The first sample is skipped: the
#    first open also starts things that live for the daemon's life (PipeWire's own
#    threads, the hunt), and those are not what a refusal owns.
read -r T2 F2 < <(echo "$OUT" | awk '$1 == "sample" && $2 == 2 { print $4, $6 }')
read -r T5 F5 < <(echo "$OUT" | awk '$1 == "sample" && $2 == 5 { print $4, $6 }')
[ -n "${T2:-}" ] && [ -n "${T5:-}" ] || fail "no samples at the 2nd and 5th refusal"
[ "$T5" -le "$T2" ] || fail "threads grew from $T2 to $T5 over three refused opens — each refusal leaves its slave engine running"
[ "$F5" -le "$F2" ] || fail "open fds grew from $F2 to $F5 over three refused opens — each refusal leaves its sockets open"
echo "OK: three refused opens after the engine started left threads ($T2 -> $T5) and fds ($F2 -> $F5) flat"
exit 0
