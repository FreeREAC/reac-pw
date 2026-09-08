#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a daemon started with NOTHING finds a segment by hearing it, takes the
# end of the pairing the wire leaves open, PUTS BOTH OF THAT SEGMENT'S NODES ON THE GRAPH,
# keeps the segment through a link flap, drops it when the link stays down, hears it again
# afterwards, and obeys a per-segment pin without a hunt.
#
# test_reac_ifscan proves the table and test_reac_hunt the verdict. This proves the lines
# that JOIN them to the sockets, the listeners and the GRAPH: the netlink fd on the main
# loop, the sniffer's classify, the serve from the 200 ms poll, the drop from the hold,
# and -- since 2026-09-08 -- that "autodetected S-1608 -> reac-capture 16 in" means a
# capture node an operator can actually patch. That line was printed on the rig over a
# graph that held no such node, for nine minutes, and nothing in this suite could see it.
#
# PRESENCE BEFORE ABSENCE: "heard" and "segment up" are asserted before any quiet claim.
#
# ISOLATION IS PART OF THE TEST, NOT A CONVENIENCE. It runs in an unprivileged user+net
# namespace AND against its own private PipeWire, because `unshare -n` isolates the wire
# and NOT the audio graph: an earlier version of this file inherited XDG_RUNTIME_DIR and
# published its fake stagebox into the operator's live graph, where a console projected
# per-segment keys for `hear0`, `desk0` and `box` and two 40-channel doors sat among the
# real desk's nodes. Every daemon it starts is killed on every exit path, for the same
# reason. Skips (77) where the namespace, iproute2 or PipeWire is unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
LOG=$(mktemp); PEER=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)

# OUR OWN GRAPH, and nothing of ours ever outlives this script.
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$PEER" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# Wait up to N seconds for a line to appear, so a slow machine costs time and not a
# false red. Returns 1 (and prints nothing) if it never appears.
wait_for() {
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -q "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

# THE GRAPH, as this daemon's own nodes: pw-dump filtered by the hearing daemon's PID, so
# the fake stagebox peers in the same namespace can never be mistaken for its work.
# One line per node: <node.name> <reac.segment> <reac.box-width> <reac.box.mac>
daemon_nodes() {
	pw-dump | python3 -c '
import json,sys
pid=int(sys.argv[1])
d=json.load(sys.stdin)
# A NODE DOES NOT CARRY THE PID; its CLIENT does. Resolving node -> client.id -> the
# client object is what makes this attribution exact, and the alternative (matching node
# names) is what let a fake stagebox pass for the daemon\x27s own work.
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    print(p.get("node.name","?"), p.get("reac.segment","(none)"),
          p.get("reac.box-width","(none)"), p.get("reac.box.mac","(none)"))
' "$1"
}

ip link add hear0 type veth peer name desk0 || exit 90
ip link set hear0 up
ip link set desk0 up

# The hearing end: no flags, an EMPTY home — nothing declares an interface.
HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || {
	echo "hearing daemon never got running"; tail -3 "$LOG"; exit 1
}
grep -q "hearing: .* Ethernet interface" "$LOG" || { echo "FAIL: no hearing banner"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] link up — listening for REAC" "$LOG" || { echo "FAIL: hear0 not sniffed"; cat "$LOG"; exit 1; }
if grep -q "\[hear0\] segment up" "$LOG"; then
	echo "FAIL: hear0 became a segment before anything was heard"; cat "$LOG"; exit 1
fi
# NOTHING IS PUBLISHED BEFORE SOMETHING IS HEARD — the log line says so, so the graph has
# to agree: no placeholder door, no node named for a segment that does not exist yet.
if [ -n "$(daemon_nodes $PID)" ]; then
	echo "FAIL: the daemon published a node before hearing anything:"; daemon_nodes $PID; exit 1
fi

# A master on the peer end: the wire now carries REAC. The sniffer's bar for "REAC gear"
# is the PROTOCOL FRAME and nothing else (reac_disco_classify: a 0x8819 frame whose
# control block verifies, or a filler — never a packet count, and since 2026-09-03 never
# a MAC's vendor prefix either). The source address below is a real Roland one only
# because it is what this rig's captures carry; the classifier would take any.
"$BIN" --live desk0 --tx desk0 --mixer m5000 --rate 96000 --name desk \
       --src-mac 00:40:ab:de:5c:01 >"$PEER" 2>&1 &
PPID2=$!
sleep 4
grep -q "\[hear0\] REAC heard" "$LOG" || { echo "FAIL: master on the peer never heard"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
grep -q "\[hear0\] segment up" "$LOG" || { echo "FAIL: heard but not served"; cat "$LOG"; exit 1; }
# AND ON THE RIGHT END OF THE PAIRING. A desk masters this wire, so the daemon joins it as
# a SLAVE and follows its pace. Nothing was configured to say so; the verdict came from
# the frames.
grep -q "\[hear0\] a desk masters this segment" "$LOG" || {
	echo "FAIL: a desk was mastering the wire and the hunt did not say so"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] segment up (slave, chosen by hearing the wire)" "$LOG" || {
	echo "FAIL: served, but not as the slave the wire called for"; cat "$LOG"; exit 1; }
# THE PROBE'S OWN POSITIVE CONTROL. Everything below asks the graph what is NOT there, and
# a graph query that silently matches nothing answers exactly like a missing node — which
# is how the first version of this check passed while listing nothing at all. Prove it can
# see a node that must exist before trusting it about one that must not.
sleep 1
daemon_nodes $PID | grep -q "^reac-capture.hear0 " || {
	echo "FAIL: the graph probe cannot see the segment's own capture node — every"
	echo "      absence it reports below would be meaningless. Nodes it did see:"
	daemon_nodes $PID; exit 1; }

# A flap shorter than the hold: the segment is kept, nothing rebuilt.
ip link set desk0 down; sleep 1
ip link set desk0 up;   sleep 2
grep -q "\[hear0\] link back inside the hold — segment kept" "$LOG" || {
	echo "FAIL: flap did not read as kept"; cat "$LOG"; exit 1; }
if [ "$(grep -c "\[hear0\] segment dropped" "$LOG")" -ne 0 ]; then
	echo "FAIL: a flap inside the hold dropped the segment"; cat "$LOG"; exit 1
fi

# Link down past the hold: the segment drops, and the interface is sniffed again when
# link returns, so the master still on the peer is heard afresh.
ip link set desk0 down; sleep 4.5
grep -q "\[hear0\] segment dropped" "$LOG" || { echo "FAIL: no drop after the hold"; cat "$LOG"; exit 1; }
ip link set desk0 up; sleep 5
[ "$(grep -c "\[hear0\] link up — listening for REAC" "$LOG")" -ge 2 ] || {
	echo "FAIL: not sniffed again after the drop"; cat "$LOG"; exit 1; }
[ "$(grep -c "\[hear0\] segment up" "$LOG")" -ge 2 ] || {
	echo "FAIL: not served again after the drop"; cat "$LOG"; exit 1; }

# ---- THE VACANT WIRE, WHICH IS THE 2026-09-08 OUTAGE IN MINIATURE. The desk goes away
# and a BOX takes its place: a daemon that was told nothing, and that has just been
# slaving to a desk, must now DRIVE the segment — hunt, grant, establish.
kill -TERM $PPID2 2>/dev/null; wait $PPID2 2>/dev/null
ip link set desk0 down; sleep 4.5        # both ends of a veth lose carrier together
ip link set desk0 up;   sleep 1
"$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] no master heard in" 15 || {
	echo "FAIL: a box on a vacant wire and the daemon never took it"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
wait_for "\[hear0\] segment up (master, chosen by hearing the wire)" 10 || {
	echo "FAIL: the wire was taken but the segment did not come up as master"; cat "$LOG"; exit 1; }
# The job, not the decision: the box it heard is ENROLLED.
wait_for "reac-master: .* -> ESTABLISHED" 20 || {
	echo "FAIL: took the wire as master but never established with the box"
	tail -20 "$LOG"; tail -5 "$PEER"; exit 1; }
wait_for "\[hear0\] autodetected S-1608" 15 || {
	echo "FAIL: established but the box was never autodetected"; tail -20 "$LOG"; exit 1; }

# ---- BOTH NODES OF THE SEGMENT ARE ON THE GRAPH, AND THEY ARE ONE THING.
# The defect this exists against, measured on the rig 2026-09-08: reac-playback.<iface>
# present, "autodetected S-1608 -> reac-capture 16 in" in the journal, and NO
# reac-capture.<iface> in the graph — every input patch on that box dead, silently, for
# nine minutes. Sized by the box and carrying the SAME reac.segment / reac.box.* on both
# halves, because a console keys a stagebox off those props and a capture node that
# belongs to no segment cannot be found by any client.
sleep 2
NODES=$(daemon_nodes $PID)
cap=$(echo "$NODES" | awk '$1 == "reac-capture.hear0"')
play=$(echo "$NODES" | awk '$1 == "reac-playback.hear0"')
[ -n "$cap" ] || { echo "FAIL: no reac-capture.hear0 on the graph"; echo "$NODES";
	grep -E "autodetected|NOT on the graph|stream ERROR|segment up|segment dropped|stream " "$LOG" | tail -25; exit 1; }
[ -n "$play" ] || { echo "FAIL: no reac-playback.hear0 on the graph"; echo "$NODES"; exit 1; }
[ "$(echo "$NODES" | awk '$1 == "reac-capture.hear0"' | wc -l)" -eq 1 ] || {
	echo "FAIL: more than one capture node for one segment"; echo "$NODES"; exit 1; }
[ "$(echo "$cap" | awk '{print $2}')" = "hear0" ] || {
	echo "FAIL: the capture node names no segment: $cap"; exit 1; }
[ "$(echo "$play" | awk '{print $2}')" = "hear0" ] || {
	echo "FAIL: the playback node names the wrong segment: $play"; exit 1; }
[ "$(echo "$cap" | awk '{print $3}')" = "16x8" ] || {
	echo "FAIL: the capture node is not sized to the box: $cap"; exit 1; }
[ "$(echo "$cap" | awk '{print $4}')" = "$(echo "$play" | awk '{print $4}')" ] || {
	echo "FAIL: the two nodes of one segment carry different box MACs"; echo "$NODES"; exit 1; }
# NO GHOST DOOR: every node this daemon published names a segment it actually serves.
while read -r name seg rest; do
	[ -z "$name" ] && continue
	case "$name" in
	  *".$seg") : ;;
	  *) echo "FAIL: node '$name' does not belong to segment '$seg'"; echo "$NODES"; exit 1 ;;
	esac
done <<< "$NODES"
# ---- AND A NODE THAT GOES AWAY COMES BACK. The recovery, driven end to end rather
# than argued: something outside the daemon destroys the capture node (`pw-cli destroy`,
# which is exactly the shape of the rig's own failure — the node is gone and the daemon's
# stream still thinks it succeeded), and the daemon has to NOTICE, say so with PipeWire's
# own reason, and rebuild it within the bounded ladder. The rig went nine minutes.
CAPID=$(pw-dump | python3 -c "
import json,sys
for o in json.load(sys.stdin):
    if o.get('type')=='PipeWire:Interface:Node' and o['info']['props'].get('node.name')=='reac-capture.hear0':
        print(o['id'])" | head -1)
[ -n "$CAPID" ] || { echo "FAIL: cannot find the capture node to destroy"; exit 1; }
pw-cli destroy "$CAPID" >/dev/null 2>&1
wait_for "reac-capture is NOT on the graph .* rebuilding it (attempt 1 of" 15 || {
	echo "FAIL: the capture node was destroyed and the daemon never noticed"
	tail -10 "$LOG"; exit 1; }
for i in $(seq 40); do
	NEWID=$(pw-dump | python3 -c "
import json,sys
for o in json.load(sys.stdin):
    if o.get('type')=='PipeWire:Interface:Node' and o['info']['props'].get('node.name')=='reac-capture.hear0':
        print(o['id'])" | head -1)
	[ -n "$NEWID" ] && [ "$NEWID" != "$CAPID" ] && break
	sleep 0.25
done
[ -n "$NEWID" ] && [ "$NEWID" != "$CAPID" ] || {
	echo "FAIL: the daemon said it was rebuilding and no new capture node appeared"
	echo "      (was $CAPID, now '${NEWID:-none}')"; tail -10 "$LOG"; exit 1; }
# ...and it is a whole node again, not a stub: same segment, same box, same width.
NODES=$(daemon_nodes $PID)
[ "$(echo "$NODES" | awk '$1 == "reac-capture.hear0" {print $2" "$3}')" = "hear0 16x8" ] || {
	echo "FAIL: the rebuilt capture node lost its identity"; echo "$NODES"; exit 1; }

# WHAT THIS RIG CANNOT PROVE, said here rather than left as a gap: the graph-clock
# reference. A pw_stream only runs when the graph drives it, no adapter node here even
# materialises its ports without a session manager, and this namespace deliberately has
# none — so nothing can be driven and no driver clock is ever seen. The DECISION (a
# hardware clock is admitted and graded, a software timer and a freewheeling graph are
# refused, an operator designation outranks the name heuristic) is pinned offline by
# test_reac_pacer_clock; that the CAPTURE node publishes that sample at all — the fix for
# a reference that used to depend on somebody patching the playback side — is visible only
# on a rig with real hardware in the graph, and is checked there.

# ---- A PIN IS SERVED WITHOUT A HUNT. `REAC_ROLE_<segment>` is an answer about this
# wire -- a setting, not a guess -- so it waits only for the wire to BE a segment. The box
# stays where it is; only the conf changes, and the segment is bounced so the sniffer
# re-reads it.
mkdir -p "$CONF/.config/reac-pw"
echo "REAC_ROLE_hear0=master" > "$CONF/.config/reac-pw/reac-pw.env"
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null
ip link set desk0 down; sleep 4.5
ip link set desk0 up;   sleep 1
"$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] REAC heard, and REAC_ROLE_hear0 pins this segment as MASTER" 15 || {
	echo "FAIL: the per-segment pin was not honoured on the first classifying frame"
	tail -20 "$LOG"; exit 1; }
wait_for "\[hear0\] segment up (master, pinned by REAC_ROLE_<segment>)" 10 || {
	echo "FAIL: served, but not reported as pinned"; tail -20 "$LOG"; exit 1; }
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null

kill -TERM $PID; wait $PID; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: clean SIGTERM exited $rc"; tail -5 "$LOG"; exit 1; }
echo "OK: heard, joined a desk as slave, kept through a flap, dropped past the hold,
    heard again, took a vacant wire as master, established with the box and put BOTH of
    its nodes on the graph, and served a per-segment pin without a hunt"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
