#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, issue #97: A SEGMENT THAT FOLLOWS NOBODY GOES BACK TO THE HUNT.
#
# Measured on the rig 2026-09-13, reac-pw 1.0.1 / libreac 1.0.2. An S-1608 in M mode was
# mastering a VLAN and the daemon deferred to it — correctly. The box was set back to S and
# power-cycled, so the foreign master left. The daemon logged `reac_slave: STATE
# ESTABLISHED -> DROP -> FLOOD_ANNOUNCE` and STAYED there: a slave courting a segment that
# no longer had a master, while the cold box waited for one that was never going to
# announce. Neither side could move. `systemctl --user restart reac-pw` brought the segment
# up as master and enrolled the box in two seconds.
#
# THE EVIDENCE IS THE SEGMENT'S OWN, AND THAT IS A MEASUREMENT (main.c, hearing_reevaluate).
# Keeping the hunt's sniffer alive on a joined segment is the obvious shape and it is wrong:
# a box master's stream is mostly FILLER, which the discovery peer lock deliberately refuses
# to treat as a sighting, so the wire looks EMPTY to that table while an enrolment is in
# progress. With that route enabled, tests/box-master-slave-join.sh measured the daemon
# retaking the wire six seconds into a live join and destroying it. What answers the
# question is reac_segment_heard: are the master's frames still arriving AND DECODING
# through this segment's own RX gate?
#
# THE SHAPE OF THE TEST. A fake box master on the far end of a veth, with the LINK LEFT UP
# throughout — a link bounce is a different path (the ifscan hold) and it is the one that
# already worked. The master simply stops, as a box being switched to S and rebooted does.
#
# PRESENCE BEFORE ABSENCE: the join is asserted first. "It came back as master" is worth
# nothing unless it was demonstrably a slave to begin with.
#
# Isolation is part of the test (tests/hearing-finds-a-segment.sh's header has the
# reasoning): an unprivileged user+net+pid namespace AND a private PipeWire, and the peer
# end of the pair lives in a nested network namespace so both ends are never ours.
# Skips (77) where the namespaces, iproute2 or PipeWire are unavailable.
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

ip link add gone0 type veth peer name gbox0 || exit 90
ip link set gbox0 netns $NSPID || exit 90

BOXMAC=00:40:ab:c4:dc:a1
# The box is on the wire before the carrier is, so the segment is a box-master join from
# the first instant of link and the masterless licence is never in the race.
$in_peer "$FAKE" gbox0 "$BOXMAC" 8 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
FAKEPID=$!
sleep 0.5
ip link set gone0 up; peer ip link set gbox0 up

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- 1. IT JOINED. Nothing below means anything without this.
wait_for_since 1 "\[gone0\] box masters this wire" 25 || {
	echo "FAIL: a box mastered the wire and the daemon never joined it"
	tail -20 "$LOG"; tail -3 "$RT/box.log"; exit 1; }
wait_for_since 1 "\[gone0\] segment up (slave, enrolling with the box that masters it" 25 || {
	echo "FAIL: the join was announced and the segment never came up as its slave"
	tail -20 "$LOG"; exit 1; }
# AND OUR ENGINE IS ACTUALLY FOLLOWING IT: frames arriving and decoding through this
# segment's gate. That latch is the very evidence hearing_reevaluate reads, so a run in
# which it never went up would make the claim below vacuous.
wait_for_since 1 "reac_rx: \[gone0\] ok=[1-9]" 25 || {
	echo "FAIL: joined, but no frame of the box master's ever decoded — the 'gone'"
	echo "      decision below would then be about a segment that never arrived"
	tail -20 "$LOG"; exit 1; }

# ---- 2. THE MASTER STOPS. The link is LEFT UP: this is a box switched to S and rebooted,
# not a cable pulled, and the cable case already worked through the ifscan hold.
MARK=$(LINE0)
kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
if ! ip link show gone0 | grep -q "state UP"; then
	echo "FAIL: the link went down when the master stopped, so this run measures the"
	echo "      ifscan hold and not #97"; ip link show gone0; exit 1
fi

# ---- 3. THE SEGMENT IS RE-DECIDED, AND IT TAKES THE WIRE. Bounded: the heard latch costs
# REAC_SEGMENT_HEARD_QUIET_TICKS (5 s) to clear and hearing_reevaluate waits the same again,
# so ~10 s, plus the fresh hunt's own masterless observation. 60 s is generous for a loaded
# machine and still cannot pass by waiting: nothing else in this daemon re-decides a joined
# segment at all.
wait_for_since "$MARK" "\[gone0\] the master this segment was following has been silent" 60 || {
	echo "FAIL: the box master stopped and the segment went on courting nobody — this is"
	echo "      #97 exactly: ESTABLISHED -> DROP -> FLOOD_ANNOUNCE for ever"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }
wait_for_since "$MARK" "\[gone0\] segment up (master," 40 || {
	echo "FAIL: the segment was re-decided and never took the wire it now has to itself"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }
# AND IT IS DRIVING: a master that is not probing is a master in the journal only.
wait_for_since "$MARK" "reac-master: .* PROBING" 30 || {
	echo "FAIL: the segment came up as master and its engine never probed"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }

# ---- 4. AND IT DID NOT DO IT WHILE THE MASTER WAS STILL THERE. The whole first phase ran
# with a box master on the wire; if the re-decision were firing on the discovery table
# rather than on the segment's own frames it would have fired then, mid-enrolment — which
# is exactly how it destroyed a live join when it was written that way.
if tail -n "+1" "$LOG" | head -n "$((MARK - 1))" | grep -q "has been silent"; then
	echo "FAIL: the segment was re-decided WHILE its master was still streaming — that is"
	echo "      the enrolment-killing shape, not the fix"
	head -n "$((MARK - 1))" "$LOG" | tail -25; exit 1
fi

echo "OK: a segment whose master left is re-decided and takes the wire (#97)"
INNER
)
rc=$?
echo "$OUT"
exit $rc
