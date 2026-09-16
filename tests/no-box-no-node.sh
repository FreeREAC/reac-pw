#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a segment with NO recognised box has NO node on the graph — and a box
# that arrives brings the pair, and a box that goes takes it away again.
# (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
# "a node with no ports is not a segment on the graph".)
#
# THE FAULT, ON THE DESK 2026-09-16. The empty untagged trunk segment — probing, box-model
# `none` — logged "MASTER autodetect — the segment's door is on the graph now
# (reac-playback, no ports yet)" and the console rendered a device reading `none / 0 in`.
# A row for a thing that is not there. The zero-port door came from Q5 option C, which
# wanted a role settable before anything enrols; that requirement is now served by
# reac-pw.conf, and the door is withdrawn.
#
# PRESENCE BEFORE ABSENCE, AND IT IS THE WHOLE STRUCTURE OF THIS TEST. Phase 2 — a real
# box enrolled, the pair on the graph, sized and labelled — is the control for phases 1
# and 3. An absence reported by an instrument that has never shown a presence is not a
# measurement, and a pw-dump filter that matches nothing looks exactly like an empty graph.
#
#   1. EMPTY WIRE. The daemon masters it and probes. Zero nodes carry reac.segment=nbn0.
#   2. A BOX ARRIVES (libreac's fake_box on the far end, in its own netns). reac-capture
#      AND reac-playback appear, both carrying the segment, with PORTS.
#   3. THE BOX GOES. Both nodes go with it — a node that outlives its box is the same
#      `none / 0 in` row arriving by the other door.
#
# ISOLATION, both halves: a user+net+mount+pid namespace with its own veth, its own sysfs
# (sysfs does not follow a network namespace — box-wakes-on-a-phy-edge.sh paid for that)
# and its own PipeWire on a private runtime dir, because `unshare -n` isolates the wire and
# not the graph.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
# NOT `${2:?}`: the meson `fake_box` option is empty by default, and a test that HARD-FAILS
# on an unset knob is a red about the machine wearing the clothes of a red about the code.
FAKE="${2:-}"
SKIP=77

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# ONE LINE PER NODE THIS DAEMON OWNS: <node.name> <reac.segment> <port count>. Resolved
# node -> client.id -> client -> application.process.id, so nothing else in this namespace
# can be mistaken for its work. The PORT COUNT is the graph's OWN n_input_ports +
# n_output_ports on the node object, not a prop the daemon writes about itself.
nodes_of() {
	pw-dump | python3 -c '
import json,sys
pid=int(sys.argv[1])
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    i=o["info"]; p=i["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    n=int(i.get("n_input_ports",0))+int(i.get("n_output_ports",0))
    print(p.get("node.name","?"), p.get("reac.segment","(none)"), n)
' "$1"
}
seg_nodes() { nodes_of "$1" | awk -v s="$2" '$2 == s'; }

ip link add nbn0 type veth peer name nbnb0 || exit 90
# The peer end is another host, with its own sysfs for the same reason ours has one.
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set nbnb0 netns $NSPID || exit 90
ip link set nbn0 up; $in_peer ip link set nbnb0 up

HOME="$CONF" "$BIN" --live nbn0 --tx nbn0 --name nbn0 --rate 96000 >"$LOG" 2>&1 &
PID=$!
sleep 4
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }

# ---- 1. THE EMPTY WIRE -----------------------------------------------------------------
echo "empty-count $(seg_nodes $PID nbn0 | wc -l)"
seg_nodes $PID nbn0 | sed 's/^/  empty-node /'
echo "empty-any-node-at-all $(nodes_of $PID | wc -l)"

# ---- 2. A BOX ARRIVES ------------------------------------------------------------------
$in_peer "$FAKE" nbnb0 60 >"$RT/box.log" 2>&1 &
FAKEPID=$!
for ((i = 0; i < 120; i++)); do
	[ "$(seg_nodes $PID nbn0 | wc -l)" -ge 2 ] && break
	sleep 0.5
done
sleep 2
echo "box-count $(seg_nodes $PID nbn0 | wc -l)"
seg_nodes $PID nbn0 | sed 's/^/  box-node /'
grep -a "autodetected" "$LOG" | sed 's/^/  box-log /' | head -2

# ---- 3. THE BOX GOES -------------------------------------------------------------------
kill -TERM $FAKEPID 2>/dev/null; kill -9 $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
for ((i = 0; i < 120; i++)); do
	[ "$(seg_nodes $PID nbn0 | wc -l)" -eq 0 ] && break
	sleep 0.5
done
echo "gone-count $(seg_nodes $PID nbn0 | wc -l)"
seg_nodes $PID nbn0 | sed 's/^/  gone-node /'
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
val() { echo "$OUT" | grep -a "^$1 " | head -1 | awk '{print $2}'; }

echo "$OUT" | grep -qa '^daemon-died' && fail "the daemon died at start"

# 0. THE CONTROL FIRST. If a box never brought nodes, this instrument has never seen a
#    presence and neither of the absences below means anything.
[ "$(val box-count)" = "2" ] \
	|| fail "a box enrolled on this wire and did NOT bring reac-capture + reac-playback (got $(val box-count)) — the instrument has shown no presence, so nothing else here is a measurement"
echo "$OUT" | grep -qa '^  box-node reac-capture' \
	|| fail "the enrolled box brought no reac-capture node: $(echo "$OUT" | grep -a '^  box-node ')"
echo "$OUT" | grep -qa '^  box-node reac-playback' \
	|| fail "the enrolled box brought no reac-playback node: $(echo "$OUT" | grep -a '^  box-node ')"
# THE PORT COUNT IS PRINTED AND NOT ASSERTED, and the reason belongs here rather than in
# a later debugging session: a private PipeWire with NO SESSION MANAGER reports 0 ports on
# these nodes for as long as they live — measured, 80 s of waiting on a segment whose
# S-4000S was enrolled and whose own journal read "reac-capture 32 in". The live graph,
# with WirePlumber, shows 32. So the number is evidence about the fixture, not about the
# daemon, and the presence CONTROL is the pair of nodes plus the daemon's own
# `autodetected ... -> reac-capture N in` line.
echo "$OUT" | grep -qa 'box-log.*autodetected' \
	|| fail "the box never enrolled, so this run has shown no presence and neither absence below is a measurement"

# 1. THE EMPTY WIRE HAS NO NODE.
[ "$(val empty-count)" = "0" ] \
	|| fail "a segment with no recognised box published $(val empty-count) node(s) — this is the 'none / 0 in' device: $(echo "$OUT" | grep -a '^  empty-node ')"

# 2. THE BOX LEAVES AND SO DO ITS NODES.
[ "$(val gone-count)" = "0" ] \
	|| fail "the box left and $(val gone-count) node(s) outlived it: $(echo "$OUT" | grep -a '^  gone-node ')"

echo "OK"
exit 0
