#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A SEGMENT A PIN NAMES HAS A DOOR ON THE GRAPH WHATEVER IS ON THE WIRE.
#
# The ruling (openmixer docs/design/specs/2026-08-20-reac-master-arbitration.md §6 Q5,
# "ANSWERED 2026-09-14 (operator): option C"; 2026-09-13-reac-plug-and-play.md §9 lane 1):
# the console invents no row and surfaces no third list — reac-pw opens a door for every
# segment it HEARS and for every segment a pin names, whatever is on the wire at that
# moment, so the console's `/reac/segment/{name}` row exists from the daemon's own props
# and a ROLE CAN BE SET ON IT BEFORE ANYTHING ENROLS.
#
# THE DEFECT IT EXISTS AGAINST, measured on the rig 2026-09-14 with reac-pw 1.0.2. Two
# segments published NO NODE AT ALL:
#   - a VLAN pinned `tap` with nothing on the wire: the tap's 1 s survey heard nothing,
#     listener_open_tap FAILED, and the whole listener was wiped. No node, no console row,
#     and the operator could not change that segment's role back;
#   - a pinned MASTER with no box: the reac-playback node IS that segment's door (it
#     carries reac.segment and accepts reac.cfg.role), and its graph filter is deferred
#     until a box declares its geometry. A cold stage therefore published nothing while
#     the daemon was driving the wire perfectly well.
# Both now stand as DOORS: one node, the segment's identity, an honest `none` for the
# master state, and no engine behind them.
#
# NOTHING IS ON THESE WIRES, AND THAT IS THE POINT. Each pair's peer end is moved to a
# nested network namespace and brought up there, so our end has carrier and the wire
# carries not one frame. The daemon may not transmit on the tap (that absence is measured,
# with its own positive control, in tests/tap-sends-nothing.sh); here the claim is that the
# NODE EXISTS.
#
# PRESENCE BEFORE ABSENCE, and it is load-bearing twice: a pw-dump filter that matches
# nothing reports exactly like a missing node, so the probe proves it can see this
# daemon's nodes and read a property off one before any per-segment claim is made.
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

# THE GRAPH, as this daemon's own nodes: pw-dump resolved node -> client.id -> client, so
# nothing else in this namespace can be mistaken for its work. One line per node:
# <node.name> <reac.segment> <reac.master.state> <reac.cfg.role.state> <reac.master.refusal>
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
    print(p.get("node.name","?"), p.get("reac.segment","(none)"),
          p.get("reac.master.state","(none)"), p.get("reac.cfg.role.state","(none)"),
          p.get("reac.master.refusal","(none)"))
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

# THE PINS, in the file the daemon reads itself. `tap` is per-segment only, which is why
# both keys are written per segment.
mkdir -p "$CONF/.config/reac-pw"
cat > "$CONF/.config/reac-pw/reac-pw.env" <<EOF
REAC_ROLE_tapdr0=tap
REAC_ROLE_mstdr0=master
EOF

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "FAIL: the daemon never got running"; tail -5 "$LOG"; exit 1; }

ip link set tapdr0 up; peer ip link set ptap0 up
ip link set mstdr0 up; peer ip link set pmst0 up

# ---- 0. THE PROBE'S POSITIVE CONTROL. A pinned MASTER drives on link with no frame
# waited for (that is settled, 2026-09-08), so SOMETHING of this daemon's must reach the
# graph within seconds whatever the doors do. If the probe cannot see one node it cannot
# testify about any, and every claim below would be a broken search reporting an absence.
for ((i = 0; i < 40; i++)); do
	[ -n "$(daemon_nodes $PID)" ] && break
	sleep 0.5
done
[ -n "$(daemon_nodes $PID)" ] || {
	echo "FAIL: the graph probe sees NO node of this daemon at all — it cannot testify"
	echo "      that a particular one is missing. Journal:"; tail -25 "$LOG"; exit 1; }

# ---- 1. A SEGMENT PINNED `tap` ON A SILENT WIRE HAS A DOOR.
wait_door $PID tapdr0 25 || {
	echo "FAIL: a VLAN pinned tap with nothing on the wire published NO node, so the"
	echo "      console has no row for it and its role cannot be changed back (Q5)."
	echo "--- this daemon's nodes:"; daemon_nodes $PID
	echo "--- journal:"; grep -E "tapdr0|TAP" "$LOG" | tail -20; exit 1; }
TAPDOOR=$(door_of $PID tapdr0)
# AND IT IS HONEST ABOUT HEARING NOTHING. `none` is the existing vocabulary for "no
# master on this wire as far as we can hear" (reac_segment_ident.h's own `heard = 0`
# arm); a door that read `foreign` here would be claiming a desk nobody heard.
[ "$(echo "$TAPDOOR" | awk '{print $3}')" = "none" ] || {
	echo "FAIL: the tap's door claims a master state of '$(echo "$TAPDOOR" | awk '{print $3}')'"
	echo "      on a wire that carries nothing: $TAPDOOR"; exit 1; }
[ "$(echo "$TAPDOOR" | awk '{print $4}')" = "role_hunting" ] || {
	echo "FAIL: the tap's door answers role state '$(echo "$TAPDOOR" | awk '{print $4}')';"
	echo "      nothing is owed and no swap failed — it is a role that cannot be"
	echo "      PERFORMED because there is nothing to perform it against: $TAPDOOR"; exit 1; }
[ "$(echo "$TAPDOOR" | awk '{print $5}')" = "none" ] || {
	echo "FAIL: the tap's door publishes a refusal on a wire it never refused: $TAPDOOR"; exit 1; }
# AND THE JOURNAL SAYS WHICH OF THE THREE IT IS DOING, said in the tap's own words rather
# than in a wire role it does not present.
grep -q "\[tapdr0\] pinned tap" "$LOG" || {
	echo "FAIL: a tap pin was not announced as one at link"; grep tapdr0 "$LOG" | tail -10; exit 1; }
grep -q "\[tapdr0\] segment PUBLISHED as a VACANT DOOR" "$LOG" || {
	echo "FAIL: the tap heard nothing and did not say it was publishing a vacant door"
	grep tapdr0 "$LOG" | tail -10; exit 1; }

# ---- 2. A PINNED MASTER WITH NO BOX HAS A DOOR, and it is reac-playback: the node that
# carries reac.segment and accepts reac.cfg.role in the master role.
wait_door $PID mstdr0 25 || {
	echo "FAIL: a pinned master with no box published NO node — the segment is driving"
	echo "      the wire and the console has no row for it (Q5)."
	echo "--- this daemon's nodes:"; daemon_nodes $PID
	echo "--- journal:"; grep -E "mstdr0" "$LOG" | tail -20; exit 1; }
MSTDOOR=$(door_of $PID mstdr0)
[ "$(echo "$MSTDOOR" | awk '{print $1}')" = "reac-playback.mstdr0" ] || {
	echo "FAIL: the master segment's door is '$(echo "$MSTDOOR" | awk '{print $1}')' and"
	echo "      not its reac-playback node — one segment, one doorway: $MSTDOOR"; exit 1; }
# EXACTLY ONE DOOR PER SEGMENT. Two nodes publishing one segment's state would be two
# doors onto one fact, which is what keying a console row on this property forbids.
[ "$(door_of $PID mstdr0 | wc -l)" -eq 1 ] || {
	echo "FAIL: more than one node carries reac.segment=mstdr0:"; door_of $PID mstdr0; exit 1; }
[ "$(door_of $PID tapdr0 | wc -l)" -eq 1 ] || {
	echo "FAIL: more than one node carries reac.segment=tapdr0:"; door_of $PID tapdr0; exit 1; }
grep -q "\[mstdr0\] pinned master — driving on link" "$LOG" || {
	echo "FAIL: the pinned master never drove on link"; grep mstdr0 "$LOG" | tail -10; exit 1; }
# AND IT ANSWERS FOR THE SEGMENT, not only for itself. The master door's aggregate is
# stamped by the badge timer rather than at create, so it is waited for — and `us` is the
# honest reading on a wire this daemon is driving with nothing else on it.
for ((i = 0; i < 40; i++)); do
	MSTDOOR=$(door_of $PID mstdr0)
	[ "$(echo "$MSTDOOR" | awk '{print $3}')" = "us" ] && break
	sleep 0.5
done
[ "$(echo "$MSTDOOR" | awk '{print $3}')" = "us" ] || {
	echo "FAIL: the master's door never published a master state; a row keyed on this"
	echo "      door would render a segment with no arbitration at all: $MSTDOOR"; exit 1; }

echo "OK: every pinned segment has a door before the wire says anything"
echo "    tap    : $TAPDOOR"
echo "    master : $MSTDOOR"
INNER
)
rc=$?
echo "$OUT"
exit $rc
