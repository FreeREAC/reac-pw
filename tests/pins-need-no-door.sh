#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A PIN NEEDS NO DOOR — a segment with nothing on its wire publishes NO
# NODE, and its role is settable all the same.
#
# THE RULING (operator, 2026-09-16): "a segment with NO recognised box must not appear in
# the PipeWire graph at all ... the probing state lives in the daemon's own row/log, not on
# the graph." The desk's own case: the empty untagged trunk VLAN logged "the segment's door
# is on the graph now (reac-playback, no ports yet)" and the console rendered a device
# reading `none / 0 in` — a row for a thing that is not there.
#
# THIS FILE IS THE INVERSE OF WHAT IT USED TO BE, and that is the honest form of a
# superseded ruling. It was doors-open-before-the-wire.sh and it proved Q5 option C
# (2026-09-14): a tap pinned on a silent wire and a pinned master with no box each had to
# publish a door, because the console's row — and therefore the role control — came off
# that node. The REQUIREMENT has not changed and is not withdrawn: a role must be settable
# before anything enrols. What changed is where it is set. reac-pw.conf answers it with no
# node at all (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md), so the
# zero-port node is cost with no remaining benefit.
#
# WHAT IS ASSERTED, in order:
#   0. the probe can SEE this daemon on the graph (its client object), or every absence
#      below is a broken search rather than a finding;
#   1. a segment pinned `tap` on a silent wire publishes NO node, and SAYS so;
#   2. a pinned master with no box publishes NO node, and SAYS so;
#   3. both pins were nevertheless OBEYED — which is the requirement Q5 served, met here
#      without a node.
#
# NOTHING IS ON THESE WIRES, AND THAT IS THE POINT. Each pair's peer end is moved to a
# nested network namespace and brought up there, so our end has carrier and the wire
# carries not one frame.
#
# Isolation is part of the test (tests/hearing-finds-a-segment.sh's header has the
# reasoning): an unprivileged user+net+pid namespace AND a private PipeWire, so nothing
# here can reach the operator's live graph or a real REAC segment. Skips (77) where the
# namespaces, iproute2 or PipeWire are unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter  >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v python3  >/dev/null 2>&1 || { echo "SKIP: no python3"; exit $SKIP; }
command -v ip       >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump  >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
LOG=$(mktemp); RT_CTL=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)

export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$RT_CTL" "$CONF" "$RT"; }
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

# THE GRAPH, as this daemon's own nodes: pw-dump resolved node -> client.id -> client, so
# nothing else in this namespace can be mistaken for its work. One line per node:
# <node.name> <reac.segment> <reac.master.state> <reac.cfg.role.state>
# <reac.master.refusal> <reac.pace.source>
daemon_nodes() {
	pw-dump | python3 -c '
import json,sys
pid=int(sys.argv[1])
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    # THE ROSTER NODE IS NOT A SEGMENT DOOR and is deliberately skipped here. Since
    # 2026-09-16 (spec amendment third, section B) this daemon always publishes ONE node for
    # ITSELF -- `reac-pw`, no ports, reac.roster=1 -- listing every segment it runs,
    # probing ones included. This probe is about the doors a SEGMENT has, and counting that
    # row as one of them would read as a ghost door on every empty wire.
    if p.get("reac.roster") is not None: continue
    print(p.get("node.name","?"), p.get("reac.segment","(none)"),
          p.get("reac.master.state","(none)"), p.get("reac.cfg.role.state","(none)"),
          p.get("reac.master.refusal","(none)"), p.get("reac.pace.source","(none)"))
' "$1"
}
# The line for the node that carries segment <seg>, or nothing.
door_of() { daemon_nodes "$1" | awk -v s="$2" '$2 == s'; }
# Wait up to N s for a segment to have a door at all.
wait_door() {  # wait_door <pid> <segment> <secs>
	local i
	for ((i = 0; i < $3 * 2; i++)); do
		[ -n "$(door_of "$1" "$2")" ] && return 0
		sleep 0.5
	done
	return 1
}

# ---- TWO WIRES WITH NOTHING ON THEM. The peer end goes to a nested namespace while it is
# still DOWN and is raised there, so our end gets carrier and the wire stays silent.
unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }

mkpair() { ip link add "$1" type veth peer name "$2" || return 1
           ip link set "$2" netns $NSPID || return 1; }
mkpair tapdr0 ptap0 || exit 90
mkpair mstdr0 pmst0 || exit 90

# THE PINS, in the ONE override file the daemon reads itself (spec 2026-09-16). A role is a
# fact about ONE wire, so there is only a per-segment form.
mkdir -p "$CONF/.config/reac-pw"
cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
[segment tapdr0]
role = tap

[segment mstdr0]
role = master
EOF

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "FAIL: the daemon never got running"; tail -5 "$LOG"; exit 1; }

ip link set tapdr0 up; peer ip link set ptap0 up
ip link set mstdr0 up; peer ip link set pmst0 up

# ---- 0. THE PROBE'S POSITIVE CONTROL, AND IT COSTS A SECOND PROCESS, because the first
# finding of this rewrite was that a daemon serving only empty segments had NO PIPEWIRE
# CLIENT AT ALL — the connection is made by the first filter, so with no node there was
# nothing of it on the graph to anchor a probe to. An absence measured against an empty
# graph is not a measurement.
#
# SINCE THE ROSTER NODE (spec amendment 2026-09-16 third, §B) that is no longer true: this
# daemon always publishes `reac-pw`, no ports, reac.roster=1, listing both of these empty
# segments. The second process is KEPT anyway, and deliberately — it is the control for the
# claim this file makes, which is about a segment's DOOR, and the roster node is not one.
# tests/roster-node-lists-every-segment.sh is where the roster is the subject.
#
# So the control is a SECOND daemon on a third silent wire with `--box s1608` PINNED. The
# spec keeps that case deliberately — a pin is the operator's statement that this box
# belongs on this wire, and its whole purpose is that the patch survives a box that is not
# powered yet — so it publishes its pair with nothing on the wire, through the SAME reader
# that is about to report two absences.
mkpair ctldr0 pctl0 || exit 90
ip link set ctldr0 up; peer ip link set pctl0 up
HOME="$CONF" "$BIN" --live ctldr0 --tx ctldr0 --name ctldr0 --box s1608 \
	>"$RT_CTL" 2>&1 &
CPID=$!
for ((i = 0; i < 60; i++)); do
	[ -n "$(daemon_nodes $CPID)" ] && break
	sleep 0.5
done
[ -n "$(daemon_nodes $CPID)" ] || {
	echo "FAIL: the graph probe sees NO node of the PINNED control daemon either, so it"
	echo "      cannot testify that anything is missing. Control journal:"
	tail -20 "$RT_CTL"; exit 1; }
echo "control: the --box pinned daemon publishes $(daemon_nodes $CPID | wc -l) node(s), read by this same probe"
daemon_nodes $CPID | sed 's/^/  control-node /'

# Give both wires time to be heard, elected and served — longer than any door would have
# taken to appear, so "still nothing" is a settled answer and not an early read.
sleep 12

# ---- 1. A SEGMENT PINNED `tap` ON A SILENT WIRE PUBLISHES NOTHING.
[ -z "$(door_of $PID tapdr0)" ] || {
	echo "FAIL: a tap with nothing on its wire published a node — this is the"
	echo "      'none / 0 in' device the 2026-09-16 ruling removes:"; door_of $PID tapdr0
	echo "--- journal:"; grep -E "tapdr0|TAP" "$LOG" | tail -20; exit 1; }
# AND IT SAYS SO. A segment that vanishes from the graph with no line in the journal is
# the same silence one level down — the daemon's log is now the ONLY place an empty
# segment exists, so the line is load-bearing, not decoration.
grep -q "\[tapdr0\] TAP with nothing to serve — NOTHING is on the graph" "$LOG" || {
	echo "FAIL: the tap published no node and did not say why"
	grep tapdr0 "$LOG" | tail -10; exit 1; }

# ---- 2. A PINNED MASTER WITH NO BOX PUBLISHES NOTHING.
[ -z "$(door_of $PID mstdr0)" ] || {
	echo "FAIL: a pinned master with no box published a node:"; door_of $PID mstdr0
	echo "--- journal:"; grep -E "mstdr0" "$LOG" | tail -20; exit 1; }
grep -q "\[mstdr0\] MASTER autodetect — PROBING, and NOTHING is on the graph" "$LOG" || {
	echo "FAIL: the probing master published no node and did not say why"
	grep mstdr0 "$LOG" | tail -10; exit 1; }

# ---- 3. AND BOTH PINS WERE OBEYED ANYWAY. This is the requirement Q5 option C existed to
# serve — a role settable before anything enrols — met with no node at all, because it is
# answered by a file the operator writes and not by a property on a device.
grep -q "\[tapdr0\] listening — role tap (reac-pw.conf)" "$LOG" || {
	echo "FAIL: a tap pin was not announced as one at link"; grep tapdr0 "$LOG" | tail -10; exit 1; }
grep -q "\[mstdr0\] listening — role master (reac-pw.conf): driving on link" "$LOG" || {
	echo "FAIL: the pinned master never drove on link"; grep mstdr0 "$LOG" | tail -10; exit 1; }

# ---- 4. AND NOTHING ELSE OF THIS DAEMON'S IS ON THE GRAPH EITHER. Two empty wires and
# nothing pinned by --box: the whole node list must be empty, or some other path is still
# publishing a segment nobody can use.
[ -z "$(daemon_nodes $PID)" ] || {
	echo "FAIL: two silent wires and this daemon still has nodes on the graph:"
	daemon_nodes $PID; exit 1; }

echo "OK: two pinned segments, two silent wires, no node on the graph, both pins obeyed"
INNER
)
rc=$?
echo "$OUT"
exit $rc
