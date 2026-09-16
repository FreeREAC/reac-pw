#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, issue #98: A NIC ONCE HEARD AS A TRUNK DOES NOT STAY ONE FOR EVER.
#
# Measured on the rig 2026-09-13, reac-pw 1.0.1. The USB NIC `enp128s20f0u2` spent an hour
# on a switch mirror port, so the daemon heard tagged REAC on it and classified it a TRUNK.
# The cable was then moved onto a stagebox directly — an S-4000M merge unit, untagged. The
# a segment pinned `role = master` logged `pinned master — driving on link`
# and immediately `untagged REAC on a trunk's native VLAN is not served; give it a tag`,
# and the box got no master until the daemon was restarted. Plug-and-play means a
# re-purposed cable works without one.
#
# WHY THE TABLE IS STILL KEPT ACROSS A BOUNCE. Deleting the topology row on carrier loss
# would destroy the VLAN netdevs a segment lives on, which the ifscan hold exists to
# preserve across a box power-cycle. So the row stays and the VERDICT is dated: the tap
# stamps its own link-up and remembers whether a tag has been heard SINCE. A trunk that is
# still a trunk transmits thousands of tagged frames a second and re-proves itself at once.
#
# PRESENCE BEFORE ABSENCE: phase 1 asserts the trunk classification and the refusal it
# causes. Without them, phase 2's success would be a daemon that never classified anything.
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

LINE0() { echo $(( $(wc -l < "$LOG") + 1 )); }
wait_for_since() {   # wait_for_since <first-line> <pattern> <secs>
	local floor="$1" pat="$2" secs="$3" i
	for ((i = 0; i < secs * 5; i++)); do
		tail -n "+$floor" "$LOG" | grep -q "$pat" && return 0
		sleep 0.2
	done
	return 1
}

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }
in_peer="nsenter -t $NSPID -n"

ip link add repat0 type veth peer name pmir0 || exit 90
ip link set pmir0 netns $NSPID || exit 90
# The mirror's tagged traffic: a box master inside vid 11 on the far end, so every frame
# reaches repat0 with an 802.1Q tag on it — which is what a switch mirror of a trunk looks
# like, and the only thing that makes a parent a trunk (reac_topo.h).
peer ip link add link pmir0 name pmir0.11 type vlan id 11 || exit 90

# PINNED MASTER, which is the issue's own configuration: the operator answered for this
# wire, and after the re-patch the pin has to be obeyed without a restart.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment repat0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"

$in_peer "$FAKE" pmir0.11 00:40:ab:c4:11:21 8 2000 >"$RT/tag.log" 2>&1 &
TAGPID=$!
sleep 0.5
ip link set repat0 up; peer ip link set pmir0 up; peer ip link set pmir0.11 up

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- 1. THE CABLE IS ON THE MIRROR: this parent is a TRUNK, and its native VLAN is not
# served. Both halves are asserted, because the second is the refusal the issue is about.
wait_for_since 1 "\[repat0\] tagged REAC heard — vid 11" 30 || {
	echo "FAIL: tagged REAC on the parent was never heard, so nothing below is about a"
	echo "      trunk at all"; tail -25 "$LOG"; tail -3 "$RT/tag.log"; exit 1; }
wait_for_since 1 "\[repat0\] this parent carries tagged REAC, so it is not itself a segment" 20 || {
	echo "FAIL: the tag was heard and the parent was never classified a trunk"
	tail -25 "$LOG"; exit 1; }
# AND THE PIN IS NOT OBEYED WHILE THAT IS TRUE, which is correct: driving a trunk parent
# puts a master on the wire beside the masters running on its own VLANs.
if tail -n +1 "$LOG" | grep -q "\[repat0\] segment up"; then
	echo "FAIL: a trunk parent was served as a segment — this run cannot show the"
	echo "      re-classification, because there was nothing to re-classify"
	grep repat0 "$LOG" | tail -20; exit 1
fi

# ---- 2. THE CABLE IS RE-PURPOSED. The tagged source stops and the link bounces, exactly
# as unplugging a cable from a mirror port and plugging it into a stagebox does.
MARK=$(LINE0)
kill -TERM $TAGPID 2>/dev/null; wait $TAGPID 2>/dev/null
ip link set repat0 down; peer ip link set pmir0 down
sleep 5                              # past the ifscan hold: the old story is over
peer ip link set pmir0 up; ip link set repat0 up

# ---- 3. THE VERDICT IS DATED, AND THE PIN IS OBEYED. No tag has arrived since this link
# came up, so the parent is an ordinary segment again and the pinned master drives it —
# untagged, with no restart. The re-proof window is REAC_HUNT_WINDOW_NS (3 s); 40 s is
# generous for a loaded machine and cannot pass by waiting, because nothing else in this
# daemon ever un-trunks a parent.
wait_for_since "$MARK" "\[repat0\] tagged REAC was heard on this parent before, and NOT ONCE since" 40 || {
	echo "FAIL: the cable was re-purposed and the parent is still a trunk — this is #98:"
	echo "      the box on it gets no master until the daemon is restarted"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }
wait_for_since "$MARK" "\[repat0\] segment up (master, pinned by reac-pw.conf)" 40 || {
	echo "FAIL: the parent was un-trunked and its pinned master still never drove it"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }
wait_for_since "$MARK" "reac-master: .* PROBING" 30 || {
	echo "FAIL: the segment came up as master and its engine never probed — a master in"
	echo "      the journal and silence on the cable"; tail -n "+$MARK" "$LOG" | tail -25; exit 1; }

echo "OK: a re-purposed cable stops being a trunk and its pinned master drives it (#98)"
INNER
)
rc=$?
echo "$OUT"
exit $rc
