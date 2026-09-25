#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, issue #102 (second half): THE RE-PROOF WINDOW RUNS WITHOUT A LINK BOUNCE.
#
# #98 dated the trunk verdict to link-up and gave it a re-proof window, and the cable-move
# case was proven by tests/trunk-classification-is-about-now.sh — which bounces the link.
# On the rig 2026-09-14 the verdict came from ONE frame at START-UP and then never
# re-proved: `tagged_since_linkup` was a LATCH, so the window could only ever be reached by
# a parent that had heard nothing at all. One frame pinned the refusal for the life of the
# process, and the operator's only fix was a restart — which re-armed the same latch.
#
# THE LINK IS NEVER TOUCHED HERE. A tagged source runs, the parent is correctly classified
# a trunk, the source stops, and that is all: the verdict has to expire on its own, because
# a trunk that is still a trunk puts thousands of tagged frames a second on the wire and
# re-proves itself continuously. Then the source comes BACK and the parent has to be a trunk
# again — a verdict that could only ever go one way would be a latch pointing the other way.
#
# WHAT THIS TEST DOES NOT ASSERT, and why. It does not require the pinned master to drive
# the parent after the expiry: the VLAN this run minted is still up with a master on it, and
# driving the parent untagged beside it is the fact-B double delivery the design forbids
# (§3). The operator's job — a cold box on a direct cable gets its master — is proven in
# tests/trunk-verdict-is-inbound-only.sh, where the cable was never a trunk at all.
#
# PRESENCE BEFORE ABSENCE: phase 1 asserts the trunk classification and the refusal. Without
# them phase 2 would pass on a daemon that never classified anything.
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
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

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

LINE0() { echo $(( $(wc -l < "$LOG") + 1 )); }
wait_for_since() {   # wait_for_since <first-line> <pattern> <secs>
	local floor="$1" pat="$2" secs="$3" i
	for ((i = 0; i < secs * 5; i++)); do
		tail -n "+$floor" "$LOG" | grep -q "$pat" && return 0
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

ip link add stale0 type veth peer name speer0 || exit 90
ip link set speer0 netns $NSPID || exit 90
peer ip link add link speer0 name speer0.11 type vlan id 11 || exit 90

# PINNED MASTER: the operator has answered for this wire, and the refusal below is what
# stopped the answer being obeyed.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment stale0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"

$in_peer "$FAKE" speer0.11 00:40:ab:c4:11:21 8 2000 >"$RT/tag.log" 2>&1 &
TAGPID=$!
sleep 0.5
ip link set stale0 up; peer ip link set speer0 up; peer ip link set speer0.11 up

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- 1. IT REALLY IS A TRUNK while the tags are on the wire, and its native VLAN is
# refused. Both halves, because the second is the refusal the issue is about.
wait_for_since 1 "\[stale0\] tagged REAC heard — vid 11" 30 || {
	echo "FAIL: tagged REAC on the parent was never heard, so nothing below is about a"
	echo "      trunk at all"; tail -25 "$LOG"; tail -3 "$RT/tag.log"; exit 1; }
wait_for_since 1 "\[stale0\] this parent carries tagged REAC, so it is not itself a segment" 20 || {
	echo "FAIL: the tag was heard and the parent was never classified a trunk"
	tail -25 "$LOG"; exit 1; }
if grep -q "\[stale0\] segment up" "$LOG"; then
	echo "FAIL: a trunk parent was served as a segment — there is nothing to re-classify"
	grep stale0 "$LOG" | tail -20; exit 1
fi

# ---- 2. THE TAGS STOP. THE LINK IS NOT TOUCHED. This is the start path: no DOWN, no UP,
# no re-open of the tap — only the passage of the re-proof window.
MARK=$(LINE0)
kill -TERM $TAGPID 2>/dev/null; wait $TAGPID 2>/dev/null

# ---- 3. THE VERDICT EXPIRES ANYWAY. The window is REAC_HUNT_WINDOW_NS (3 s); 40 s is
# generous for a loaded machine and cannot pass by waiting, because a latched verdict never
# expires at all.
wait_for_since "$MARK" "\[stale0\] tagged REAC was heard on this parent before, and NOT ONCE since" 40 || {
	echo "FAIL (#102): the tags stopped and the parent is still a trunk — the verdict is"
	echo "      latched, so #98's re-proof never runs without a link bounce and a cable"
	echo "      that heard one frame is refused for the life of the process"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }

# ---- 4. AND IT COMES BACK. The tags return, the link is still untouched, and the parent is
# a trunk again — the window is a question asked continuously, not a latch in either
# direction. Without this the expiry above could be a one-way door.
MARK2=$(LINE0)
$in_peer "$FAKE" speer0.11 00:40:ab:c4:11:21 8 2000 >"$RT/tag2.log" 2>&1 &
TAGPID2=$!
wait_for_since "$MARK2" "\[stale0\] this parent carries tagged REAC, so it is not itself a segment" 40 || {
	echo "FAIL: the tags came back and the parent was never a trunk again — the verdict"
	echo "      expired into a state it cannot leave"
	tail -n "+$MARK2" "$LOG" | tail -25; tail -3 "$RT/tag2.log"; exit 1; }

# ---- 5. AND EXPIRES AGAIN. Same cable, same link, second cycle.
MARK3=$(LINE0)
kill -TERM $TAGPID2 2>/dev/null; wait $TAGPID2 2>/dev/null
wait_for_since "$MARK3" "\[stale0\] tagged REAC was heard on this parent before, and NOT ONCE since" 40 || {
	echo "FAIL: the verdict expired once and could not expire again"
	tail -n "+$MARK3" "$LOG" | tail -25; exit 1; }

echo "OK: a trunk verdict is re-proved and expires on the start path, no link bounce (#102)"
INNER
)
rc=$?
echo "$OUT"
exit $rc
