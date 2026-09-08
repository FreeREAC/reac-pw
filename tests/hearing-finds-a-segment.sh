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
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
BIN="$1"
FAKE="$2"
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

# ONE NODE'S OWN PROPERTIES, and its port count. The refusal and the joined box master are
# both PUBLISHED FACTS -- a console renders the remedy from these keys -- so the proof reads
# the keys rather than the journal line that claims them.
# ONE NODE'S PUBLISHED ANSWER, field by field, '|'-separated (a description has spaces in
# it). THE WIDTH IS READ FROM THE NODE'S OWN DESCRIPTION, not from a port count: no
# session manager runs in this namespace, so no node here ever materialises its ports --
# every node in this graph reports zero, which reads exactly like a node with none. The
# description is composed from the very argument that sizes the ports
# (reac_source_node_new), and it is how the master phase above proves its width too.
# Prints: state|rival.kind|refusal|master.mac|segment|node.description
daemon_node_props() {
	pw-dump | python3 -c '
import json,sys
pid, want = int(sys.argv[1]), sys.argv[2]
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    if p.get("node.name")!=want: continue
    print("|".join([p.get("reac.master.state","(none)"),
                    p.get("reac.master.rival.kind","(none)"),
                    p.get("reac.master.refusal","(none)"),
                    p.get("reac.master.mac","(none)"),
                    p.get("reac.segment","(none)"),
                    p.get("node.description","(none)")]))
' "$1" "$2"
}
fld() { echo "$1" | cut -d'|' -f"$2"; }

# ---- THE PEER IS ANOTHER HOST, AND HAS TO BE ONE. Every phase below turns on what one
# side of a wire does when the OTHER side is silent, and a veth pair whose two ends both
# sit in this namespace has no other side: the daemon sniffs both, hears its own peer
# daemon's OUTGOING frames on the peer's own NIC (AF_PACKET delivers those), serves the
# peer end as a segment of its own, and then drives the very wire the phase is asking it
# to find empty. That is not a defect in the daemon -- both ends really are its own here
# -- but it makes the question unanswerable. So the peer ends live in a NESTED network
# namespace from now on: moved there while still DOWN, so the daemon never sees them at
# all, and driven with nsenter.
unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }
# BACKGROUND JOBS DO NOT GO THROUGH THE FUNCTION. `peer cmd &` backgrounds a SUBSHELL, so
# $! is the subshell's pid and `kill $!` leaves the daemon inside it running -- which is
# how a "killed" desk went on mastering hear0 through three later phases and made the
# vacant-wire assertion unreachable. nsenter with only -n EXECS its command, so the pid
# below is the daemon's own.
in_peer="nsenter -t $NSPID -n"
# Create a pair and hand the peer end over before either end ever has carrier, and leave
# BOTH ends down: link is what the daemon acts on, so a down pair is invisible to it.
mkpair() {   # mkpair <ours> <theirs>
	ip link add "$1" type veth peer name "$2" || return 1
	ip link set "$2" netns $NSPID || return 1
}
# NOTHING BELOW EVER DELETES AN INTERFACE. Linux recycles ifindexes, so a phase that
# deleted its pair and the next that created one could be handed the same index — and a
# socket somewhere still bound to it then transmits onto the new wire. That is what made
# cold1 hear a 40-channel stream from an address nobody on its segment owns, inside the
# RPM's %check, on code that passed three runs standing alone. A phase raises its own
# link and lowers it again; the indexes are fixed for the whole run.
up_pair()   { ip link set "$1" up;   peer ip link set "$2" up; }
down_pair() { ip link set "$1" down; peer ip link set "$2" down; }

mkpair pin0  pbox0  || exit 90
mkpair boxm0 mbox0  || exit 90
mkpair pinm0 mbox1  || exit 90
mkpair cold0 kbox0  || exit 90
mkpair cold1 kdesk1 || exit 90

# ---- THE PEER'S OWN EAR. Everything below asserts what left THIS daemon and landed on
# the other end of the wire, so the other end needs a capture of its own. AF_PACKET in the
# peer namespace, counting 0x8819 frames per SOURCE MAC into a file it replaces atomically,
# so the shell never reads a half-written one. Counting rather than logging is what lets it
# sit under a desk's 8000 fps flood without becoming the bottleneck.
cat > "$RT/sniff.py" <<'PYEOF'
import collections, os, socket, sys, time
iface, out = sys.argv[1], sys.argv[2]
# ETH_P_ALL, NOT 0x8819, AND THE FILTER IS OURS. A socket bound to a specific EtherType
# registers on ptype_base and is handed RECEIVED frames only; locally generated OUTGOING
# frames are delivered to ptype_all listeners alone. Bound to 0x8819 this capture saw
# every frame arriving at the peer and NOT ONE the peer itself sent -- which is why the
# yield phase read "the desk transmitted nothing" while the daemon was plainly hearing it.
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
s.bind((iface, 0))
s.settimeout(0.2)
c, last = collections.Counter(), 0.0
while True:
    try:
        d = s.recv(2048)
        if len(d) >= 14 and d[12] == 0x88 and d[13] == 0x19:
            # KEYED BY SOURCE **AND DIRECTION-KIND**, because "how many frames did they
            # send" cannot tell a master from a slave: both transmit at the wire cadence
            # from the same NIC address. A master BROADCASTS its downstream; a slave
            # unicasts its return to the master it learned. The dst is the discriminator.
            kind = "b" if d[0:6] == b"\xff\xff\xff\xff\xff\xff" else "u"
            c[d[6:12].hex() + "-" + kind] += 1
    except socket.timeout:
        pass
    now = time.time()
    if now - last > 0.3:
        last = now
        with open(out + ".tmp", "w") as f:
            for k, v in c.items():
                f.write("%s %d\n" % (k, v))
        os.replace(out + ".tmp", out)
PYEOF
# frames whose key starts with $3 ("" = every key), 0 when the file has nothing yet.
# A key is "<srcmac>-b" (broadcast) or "<srcmac>-u" (unicast), so "<mac>" counts a host's
# whole output and "<mac>-b" counts only what it BROADCASTS -- which is what mastering is.
seen() { [ -s "$2" ] || { echo 0; return; }; awk -v m="$3" 'index($1, m) == 1 {n += $2} END {print n+0}' "$2"; }
# every source MAC the peer has heard, one per line
srcs() { [ -s "$1" ] && awk '{print substr($1, 1, 12)}' "$1" | sort -u; }
# frames from every source EXCEPT one -- the positive control's counter. Naming the
# desk's MAC here instead would make the control depend on a second assumption about
# what the peer daemon puts in its L2 source; "everything that is not us" needs none.
other() { [ -s "$1" ] || { echo 0; return; }; awk -v m="$2" 'index($1, m) != 1 {n += $2} END {print n+0}' "$1"; }

mkpair hear0 desk0 || exit 90
up_pair hear0 desk0

# A MASTER IS ON THIS WIRE BEFORE THE DAEMON EVER SEES IT, which is the ordinary case and
# the one that must not be disturbed: a desk is already streaming when we get carrier. It
# is started FIRST for exactly that reason -- since the operator's "no traffic, no master"
# ruling an unpinned wire that is SILENT is taken (that is what the cold0 phase proves),
# so a phase about joining a desk has to put the desk there first or it is a phase about
# something else. The sniffer's bar for "REAC gear" is the PROTOCOL FRAME and nothing else
# (reac_disco_classify: a 0x8819 frame whose control block verifies, or a filler -- never a
# packet count, and since 2026-09-03 never a MAC's vendor prefix either). The source
# address below is a real Roland one only because it is what this rig's captures carry.
$in_peer "$BIN" --live desk0 --tx desk0 --mixer m5000 --rate 96000 --name desk \
       --src-mac 00:40:ab:de:5c:01 >"$PEER" 2>&1 &
PPID2=$!
sleep 3

# The hearing end: no flags, an EMPTY home — nothing declares an interface.
HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 4
kill -0 $PID 2>/dev/null || {
	echo "hearing daemon never got running"; tail -3 "$LOG"; exit 1
}
grep -q "hearing: .* Ethernet interface" "$LOG" || { echo "FAIL: no hearing banner"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] unpinned — listening for REAC" "$LOG" || { echo "FAIL: hear0 not sniffed"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] REAC heard" "$LOG" || { echo "FAIL: master on the peer never heard"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
grep -q "\[hear0\] segment up" "$LOG" || { echo "FAIL: heard but not served"; cat "$LOG"; exit 1; }
# AND IT WAS NEVER TAKEN. A wire with a master on it is not silent, so the masterless
# licence must never have been granted here. This is an ABSENCE claim and it gets its
# positive control at the end of the file, where the same string is REQUIRED to have
# appeared for cold0 -- a grep that cannot match is indistinguishable from a daemon that
# behaved.
if grep -q "\[hear0\] no REAC heard in" "$LOG"; then
	echo "FAIL: a desk was streaming on hear0 and the daemon called the wire silent"
	cat "$LOG"; exit 1
fi
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
peer ip link set desk0 down; sleep 1
peer ip link set desk0 up;   sleep 2
grep -q "\[hear0\] link back inside the hold — segment kept" "$LOG" || {
	echo "FAIL: flap did not read as kept"; cat "$LOG"; exit 1; }
if [ "$(grep -c "\[hear0\] segment dropped" "$LOG")" -ne 0 ]; then
	echo "FAIL: a flap inside the hold dropped the segment"; cat "$LOG"; exit 1
fi

# Link down past the hold: the segment drops, and the interface is sniffed again when
# link returns, so the master still on the peer is heard afresh.
peer ip link set desk0 down; sleep 4.5
grep -q "\[hear0\] segment dropped" "$LOG" || { echo "FAIL: no drop after the hold"; cat "$LOG"; exit 1; }
peer ip link set desk0 up; sleep 5
[ "$(grep -c "\[hear0\] unpinned — listening for REAC" "$LOG")" -ge 2 ] || {
	echo "FAIL: not sniffed again after the drop"; cat "$LOG"; exit 1; }
[ "$(grep -c "\[hear0\] segment up" "$LOG")" -ge 2 ] || {
	echo "FAIL: not served again after the drop"; cat "$LOG"; exit 1; }

# ---- THE VACANT WIRE, WHICH IS THE 2026-09-08 OUTAGE IN MINIATURE. The desk goes away
# and a BOX takes its place: a daemon that was told nothing, and that has just been
# slaving to a desk, must now DRIVE the segment — hunt, grant, establish.
kill -TERM $PPID2 2>/dev/null; wait $PPID2 2>/dev/null
peer ip link set desk0 down; sleep 4.5   # both ends of a veth lose carrier together
peer ip link set desk0 up;   sleep 1
$in_peer "$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
# EITHER ROUTE TO THE WIRE IS CORRECT HERE and which one runs is a race we do not need to
# win: the box floods broadcast FILLER on ITS PHY-up, so if that flood lands inside the
# daemon's 500 ms masterless observation the licence is cancelled and the ordinary
# vacant-wire path takes over ("no master heard in 3 s and a box is present"); if the box
# is slower to start, the wire is proven silent and taken on the licence. The phase's real
# assertion is the next one -- the segment comes up as MASTER -- and everything after it.
for i in $(seq 75); do
	grep -qE "\[hear0\] (no master heard in|no REAC heard in .* taking it as MASTER)" "$LOG" && break
	sleep 0.2
done
grep -qE "\[hear0\] (no master heard in|no REAC heard in .* taking it as MASTER)" "$LOG" || {
	echo "FAIL: the desk went away, a box is on the wire, and the daemon took neither route"
	cat "$LOG"; tail -5 "$PEER"; exit 1; }
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
# A REBUILD THAT WORKED SAYS SO. "rebuilding it" followed by silence is what a segment
# that is still broken looks like, which is the same silence this path exists to end.
wait_for "reac-capture is back on the graph (attempt 1)" 10 || {
	echo "FAIL: the node came back and the daemon never said so"; tail -10 "$LOG"; exit 1; }
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
peer ip link set desk0 down; sleep 4.5
peer ip link set desk0 up;   sleep 1
$in_peer "$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] REAC_ROLE_hear0 pins this segment as MASTER — driving on link" 15 || {
	echo "FAIL: the per-segment pin was not honoured on the first classifying frame"
	tail -20 "$LOG"; exit 1; }
wait_for "\[hear0\] segment up (master, pinned by REAC_ROLE_<segment>)" 10 || {
	echo "FAIL: served, but not reported as pinned"; tail -20 "$LOG"; exit 1; }
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null

# ---- A PINNED MASTER DRIVES ON LINK, WITH A SILENT PEER. The 2026-09-08 22:10 defect
# exactly: nothing on the far end says anything, ever, until a master announces to it, so
# a pinned master that waits for a classifying frame waits forever. Here the peer is a
# bare veth end -- no daemon, no box, not one byte -- and the assertion is that OUR
# announce is on the wire within 2 s of LINK. The peer's capture is what proves it left
# this host; a journal line would only prove we decided to.
mkdir -p "$CONF/.config/reac-pw"
echo "REAC_ROLE_pin0=master" >> "$CONF/.config/reac-pw/reac-pw.env"
up_pair pin0 pbox0
$in_peer python3 "$RT/sniff.py" pbox0 "$RT/pin0.cnt" & SNIFF1=$!
wait_for "\[pin0\] pinned master — driving on link" 10 || {
	echo "FAIL: a pinned master did not say it was driving on link"; tail -20 "$LOG"; exit 1; }
for i in $(seq 20); do [ "$(seen x "$RT/pin0.cnt" "")" -gt 0 ] && break; sleep 0.1; done
[ "$(seen x "$RT/pin0.cnt" "")" -gt 0 ] || {
	echo "FAIL: a pinned master on a silent wire put NOTHING on that wire within 2 s of"
	echo "      link -- which is the 2026-09-08 outage: it is waiting to be spoken to by"
	echo "      a box that cannot speak first."; tail -20 "$LOG"; exit 1; }
# ...and the box that could not have spoken first now answers, and establishes.
$in_peer "$BIN" --live pbox0 --tx pbox0 --role slave --box-channels 16 --name pbox \
       --src-mac 00:40:ab:c4:80:42 >"$PEER" 2>&1 &
PBOXPID=$!
wait_for "\[pin0\] segment up (master, pinned by REAC_ROLE_<segment>)" 15 || {
	echo "FAIL: pinned master never served pin0"; tail -20 "$LOG"; exit 1; }
kill -TERM $SNIFF1 2>/dev/null; wait $SNIFF1 2>/dev/null
kill -TERM $PBOXPID 2>/dev/null; wait $PBOXPID 2>/dev/null
down_pair pin0 pbox0

# ---- AN UNPINNED WIRE PROVEN SILENT IS DRIVEN, AND THAT IS WHAT WAKES A COLD BOX.
# A final-user system has NO pins on its first boot, so the pin above cannot be the whole
# answer. AND A LONE ANNOUNCE IS NOT THE ANSWER EITHER: measured on the rig 2026-09-08
# with 0.5.0-3, an unpinned enp128s20f0u2 knocked, tx rose by two frames per six seconds,
# and rx stayed at ZERO for over a minute -- a cold box answers a master that is DRIVING,
# the continuous probing stream, which is what the pinned path and 0.4.8 both send. So an
# interface observed masterless for REAC_KNOCK_LISTEN_NS (a master fills every audio slot
# and cannot be present and silent) takes the wire through the ordinary master role.
#
# The peer here is a bare veth end -- no daemon, no box, not one byte -- so what the
# capture sees is ours and nothing prompted it.
up_pair cold0 kbox0
$in_peer python3 "$RT/sniff.py" kbox0 "$RT/cold0.cnt" & SNIFF2=$!
wait_for "\[cold0\] unpinned — listening for REAC" 10 || {
	echo "FAIL: cold0 never came up as an unpinned sniffer"; tail -20 "$LOG"; exit 1; }
wait_for "\[cold0\] no REAC heard in .* taking it as MASTER" 10 || {
	echo "FAIL: an unpinned wire proven silent was never taken"; tail -20 "$LOG"; exit 1; }
wait_for "\[cold0\] segment up (master, chosen by hearing the wire)" 15 || {
	echo "FAIL: the wire was taken on silence and the segment never came up"
	tail -20 "$LOG"; exit 1; }
# A STREAM, NOT A FRAME. The whole rig correction is here: what reaches the box has to be
# a master's continuous cadence. One announce every two seconds was ~0.5 frames/s and woke
# nothing; a driving master is thousands. Measured on the peer's own capture over 1 s.
sleep 1
D0=$(seen x "$RT/cold0.cnt" ""); sleep 1; D1=$(seen x "$RT/cold0.cnt" "")
[ "$((D1 - D0))" -gt 500 ] || {
	echo "FAIL: cold0 was taken as master but only $((D1 - D0)) frames/s reach the peer --"
	echo "      that is a knock, not a master driving, and a cold box does not answer it"
	tail -20 "$LOG"; exit 1; }
# THE COLD BOX ANSWERS THE STREAM. It is started only now, so it could not have begun this
# exchange; and it is started with its PHY already up on a wire that is ALREADY being
# driven, which is the rig's own case (boxes powered before the daemon).
$in_peer "$BIN" --live kbox0 --tx kbox0 --role slave --box-channels 16 --name kbox \
       --src-mac 00:40:ab:c4:80:43 >"$PEER" 2>&1 &
KBOXPID=$!
wait_for "reac-master: .* -> ESTABLISHED" 25 || {
	echo "FAIL: driving a silent wire never established with the box that answered"
	tail -20 "$LOG"; tail -5 "$PEER"; exit 1; }
wait_for "\[cold0\] autodetected S-1608" 20 || {
	echo "FAIL: established on cold0 but the box was never autodetected"; tail -20 "$LOG"; exit 1; }
kill -TERM $SNIFF2 2>/dev/null; wait $SNIFF2 2>/dev/null
kill -TERM $KBOXPID 2>/dev/null; wait $KBOXPID 2>/dev/null
down_pair cold0 kbox0

# ---- A DESK TURNS UP ON A WIRE WE TOOK ON SILENCE: WE YIELD, WE NEVER FIGHT. The other
# half of the safety argument, and the one that costs something. Driving is a BET that
# nothing was there; a desk powered up second proves the bet wrong, and two masters on one
# segment is the fault the seglock exists to make impossible between our own processes --
# it is no better against a real desk. So the sniffer is KEPT on a wire taken this way and
# the segment is handed over: master down, slave up, no shouting.
up_pair cold1 kdesk1
$in_peer python3 "$RT/sniff.py" kdesk1 "$RT/cold1.cnt" & SNIFF3=$!
wait_for "\[cold1\] no REAC heard in .* taking it as MASTER" 10 || {
	echo "FAIL: cold1 was never taken on silence"; tail -20 "$LOG"; exit 1; }
wait_for "\[cold1\] segment up (master, chosen by hearing the wire)" 15 || {
	echo "FAIL: cold1 was taken and never served"; tail -20 "$LOG"; exit 1; }
# We are driving. Our own MAC on the wire is the one the peer sees most of.
sleep 1
OURMAC=$(awk '{print substr($1, 1, 12), $2}' "$RT/cold1.cnt" | sort -k2 -n | tail -1 | awk '{print $1}')
[ -n "$OURMAC" ] || { echo "FAIL: nothing of ours reached kdesk1"; tail -20 "$LOG"; exit 1; }
[ "$(seen x "$RT/cold1.cnt" "$OURMAC-b")" -gt 500 ] || {
	echo "FAIL: cold1 was served as master and is not BROADCASTING a downstream"
	cat "$RT/cold1.cnt"; tail -20 "$LOG"; exit 1; }
$in_peer "$BIN" --live kdesk1 --tx kdesk1 --mixer m5000 --rate 96000 --name kdesk \
       --src-mac 00:40:ab:de:5c:02 >"$PEER" 2>&1 &
KDESKPID=$!
wait_for "\[cold1\] a desk masters this segment .* yielding the master role" 25 || {
	echo "FAIL: a desk took the wire we were driving and we did not yield"
	tail -25 "$LOG"; tail -5 "$PEER"; exit 1; }
wait_for "\[cold1\] segment up (slave, chosen by hearing the wire)" 15 || {
	echo "FAIL: yielded, but never came back up as the slave"; tail -25 "$LOG"; exit 1; }
# AND WE STOPPED DRIVING. A yield that leaves our pacer on the wire is two masters with a
# polite log line.
#
# COUNTING OUR FRAMES CANNOT ANSWER THIS: a slave transmits at the wire cadence too, from
# the same NIC address, and it first spends a bounded ~5460-frame BROADCAST cold-connect
# flood (reac_fsm.h) exactly as a real box does. What separates the roles is the
# DESTINATION -- a master broadcasts its downstream, a slave unicasts its return to the
# master it learned -- so the bar is our BROADCAST rate, measured after the flood is spent
# (5460 frames at 8000 fps is ~0.7 s; 3 s is comfortably past it).
# THE POSITIVE CONTROL IS NOT OPTIONAL: this is an ABSENCE claim, and a capture that has
# died reports absence exactly like a daemon that has stopped. The desk's frames must be
# piling up on the SAME capture over the SAME window.
sleep 3
BEFORE=$(seen x "$RT/cold1.cnt" "$OURMAC-b"); DESKBEFORE=$(other "$RT/cold1.cnt" "$OURMAC")
sleep 4
AFTER=$(seen x "$RT/cold1.cnt" "$OURMAC-b");  DESKAFTER=$(other "$RT/cold1.cnt" "$OURMAC")
[ "$DESKAFTER" -gt "$((DESKBEFORE + 1000))" ] || {
	echo "FAIL: the peer capture is not receiving (frames from everyone but us:"
	echo "      $DESKBEFORE -> $DESKAFTER), so it cannot testify that we stopped driving"
	cat "$RT/cold1.cnt"; tail -20 "$LOG"; exit 1; }
# Four seconds of mastering is ~32000 broadcasts at 96 k. A yielded segment broadcasts
# nothing at all, and the bar leaves room for a stray frame rather than demanding zero.
[ "$((AFTER - BEFORE))" -lt 500 ] || {
	echo "FAIL: we yielded the segment in the journal and BROADCAST $((AFTER - BEFORE))"
	echo "      frames in 4 s anyway -- that is still a master over a desk"; exit 1; }
kill -TERM $SNIFF3 2>/dev/null; wait $SNIFF3 2>/dev/null
kill -TERM $KDESKPID 2>/dev/null; wait $KDESKPID 2>/dev/null
down_pair cold1 kdesk1

# ---- A BOX MASTERS AN UNPINNED WIRE: WE JOIN IT, AT ITS OWN WIDTH (0.5.1).
# The 2026-09-09 rig proof in miniature, with the ruling applied: an S-0808 on M is not a
# hazard to refuse, it is a clock to follow. The peer is the fake box master -- broadcast
# box geometry, one master-only record a second, no handshake, which is what a stagebox on
# M really does (reac-protocol/wire-format.md) -- and it is started BEFORE the link comes
# up, so the wire carries a master from the first instant of carrier and the masterless
# licence is not in the race at all.
BOXMAC=00:40:ab:c4:08:bc
$in_peer "$FAKE" mbox0 "$BOXMAC" 8 2000 >"$RT/boxm.log" 2>&1 &
FAKEPID=$!
sleep 0.5
up_pair boxm0 mbox0
# THE CAPTURE GOES UP WITH THE CARRIER, never before it: a recv on a down interface ends
# with ENETDOWN and leaves a probe that reports absence because it died.
$in_peer python3 "$RT/sniff.py" mbox0 "$RT/boxm0.cnt" & SNIFF4=$!
wait_for "\[boxm0\] box masters this wire — joining it as a slave (operator rule: a box that wants to be master gets the clock)" 15 || {
	echo "FAIL: a box mastered an unpinned wire and the daemon did not join it"
	tail -20 "$LOG"; tail -3 "$RT/boxm.log"; exit 1; }
wait_for "\[boxm0\] segment up (slave, receive-only on a box master, chosen by hearing the wire)" 15 || {
	echo "FAIL: joined in the journal, but the segment never came up receive-only"
	tail -20 "$LOG"; exit 1; }
# THE JOB: the box's channels are on the graph, sized by what the box announced -- 8, not
# a 40-slot fabric with 32 rows of silence -- and the node says whose clock they are on.
sleep 1.5
BP=$(daemon_node_props $PID reac-capture.boxm0)
[ -n "$BP" ] || { echo "FAIL: no reac-capture.boxm0 on the graph after joining a box master"
	daemon_nodes $PID; tail -20 "$LOG"; exit 1; }
[ "$(fld "$BP" 6)" = "REAC 8ch capture (box mic inputs)" ] || {
	echo "FAIL: the joined segment is not sized to the box's 8 ch, and does not say it is"
	echo "      reading the box's own geometry: $BP"; exit 1; }
[ "$(fld "$BP" 1)" = "foreign" ] || { echo "FAIL: master.state is not foreign: $BP"; exit 1; }
[ "$(fld "$BP" 2)" = "box" ] || { echo "FAIL: master.rival.kind is not box: $BP"; exit 1; }
[ "$(fld "$BP" 3)" = "none" ] || {
	echo "FAIL: we JOINED it, so master.refusal must be none: $BP"; exit 1; }
[ "$(fld "$BP" 4)" = "$BOXMAC" ] || {
	echo "FAIL: the joined node names a master other than the box: $BP"; exit 1; }
[ "$(fld "$BP" 5)" = "boxm0" ] || { echo "FAIL: the joined node names another segment: $BP"; exit 1; }
# AND WE PUT NOTHING ON THAT WIRE. A box on M runs no handshake, so a slave engine
# flooding at it would be noise: the join is receive-only. This is an ABSENCE claim, so its
# positive control is the SAME capture counting the box's own frames over the same window.
sleep 1
OURS_B=$(other "$RT/boxm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXFR=$(seen x "$RT/boxm0.cnt" "$(echo $BOXMAC | tr -d :)")
[ "$BOXFR" -gt 500 ] || {
	echo "FAIL: the peer capture has only $BOXFR frames from the box master itself, so it"
	echo "      cannot testify that we sent nothing"; cat "$RT/boxm0.cnt"; exit 1; }
[ "$OURS_B" -lt 50 ] || {
	echo "FAIL: a receive-only join put $OURS_B frames on the wire -- there is nothing on"
	echo "      the far end that could answer them"; cat "$RT/boxm0.cnt"; exit 1; }
kill -TERM $SNIFF4 2>/dev/null; wait $SNIFF4 2>/dev/null
kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
down_pair boxm0 mbox0

# ---- THE SAME BOX ON A WIRE PINNED MASTER: REFUSED, AND THE REFUSAL IS PUBLISHED.
# Two answers that contradict each other -- the operator wrote MASTER on this wire and a
# stagebox is mastering it -- and the daemon never settles that by out-shouting a box. What
# it must not do is vanish: on the rig the refused wire published nothing at all, so the
# console had an absence to render and no remedy to show.
#
# THE PIN STILL DRIVES FIRST, and that is not a defect to test around. A pinned master is
# served on LINK with no frame waited for, because a cold stagebox in slave mode transmits
# nothing until a master announces to it (the 2026-09-08 outage). So the daemon cannot know
# a box is mastering this wire until it has listened, and its own engine -- which classifies
# every frame on that wire -- is what tells it, about a second later. The phase asserts the
# END STATE and the STOP: the segment goes down, a door goes up, and nothing more of ours
# reaches the far end.
echo "REAC_ROLE_pinm0=master" >> "$CONF/.config/reac-pw/reac-pw.env"
$in_peer "$FAKE" mbox1 "$BOXMAC" 8 2000 >"$RT/boxm1.log" 2>&1 &
FAKEPID2=$!
sleep 0.5
up_pair pinm0 mbox1
$in_peer python3 "$RT/sniff.py" mbox1 "$RT/pinm0.cnt" & SNIFF5=$!
wait_for "\[pinm0\] REFUSED (rival-master-box): REAC_ROLE_pinm0 pins this segment MASTER" 20 || {
	echo "FAIL: a pinned master beside a box on M did not refuse"
	grep -n "pinm0" "$LOG" | tail -20; tail -3 "$RT/boxm1.log"
	echo "--- conf:"; cat "$CONF/.config/reac-pw/reac-pw.env"; exit 1; }
wait_for "\[pinm0\] segment REFUSED and PUBLISHED (door only, 8-ch rival)" 15 || {
	echo "FAIL: refused, and then published nothing -- which is the 2026-09-09 defect"
	tail -20 "$LOG"; exit 1; }
sleep 1.5
RP=$(daemon_node_props $PID reac-capture.pinm0)
[ -n "$RP" ] || { echo "FAIL: a refused wire published NO door node at all"
	daemon_nodes $PID; tail -20 "$LOG"; exit 1; }
[ "$(fld "$RP" 1)" = "foreign" ] || {
	echo "FAIL: the door does not say master.state=foreign: $RP"; exit 1; }
[ "$(fld "$RP" 2)" = "box" ] || { echo "FAIL: the door does not say rival.kind=box: $RP"; exit 1; }
[ "$(fld "$RP" 3)" = "rival-master-box" ] || {
	echo "FAIL: the door does not carry the refusal code: $RP"; exit 1; }
[ "$(fld "$RP" 4)" = "$BOXMAC" ] || {
	echo "FAIL: the door names a master other than the rival $BOXMAC: $RP"; exit 1; }
[ "$(fld "$RP" 5)" = "pinm0" ] || { echo "FAIL: the door names another segment: $RP"; exit 1; }
# A DOOR IS NOT AN ENGINE: no playback node, because nothing is driving this wire.
if daemon_nodes $PID | grep -q "^reac-playback.pinm0 "; then
	echo "FAIL: a refused wire published a reac-playback node -- a door onto an engine"
	echo "      that is not there"; daemon_nodes $PID; exit 1
fi
# AND THE TRANSMISSION STOPPED. A pin is served ON LINK -- a cold box cannot speak first,
# so the daemon drives before it can possibly know a box is mastering this wire -- and what
# the refusal has to prove is therefore not "never transmitted" but "STOPPED, and stayed
# stopped". Measured as a DELTA over a window that begins after the door went up, with the
# box's own frames on the same capture over the same window as the positive control: a
# capture that has died reports absence exactly like a daemon that has stopped.
BEFORE_P=$(other "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXB=$(seen x "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
sleep 3
AFTER_P=$(other "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXA=$(seen x "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
[ "$BOXA" -gt "$((BOXB + 1000))" ] || {
	echo "FAIL: the box's own frames went $BOXB -> $BOXA on this capture, so it is not"
	echo "      receiving and cannot testify that we stopped"; cat "$RT/pinm0.cnt"; exit 1; }
[ "$((AFTER_P - BEFORE_P))" -lt 50 ] || {
	echo "FAIL: the wire was refused and a door published, and we put"
	echo "      $((AFTER_P - BEFORE_P)) more frames on it in 3 s anyway"
	cat "$RT/pinm0.cnt"; exit 1; }
# ---- AND THE REFUSAL IS NOT A LATCH. The switch is moved to slave: the box stops
# mastering, its sighting ages out, and the wire the operator pinned is driven after all --
# without a restart, which is what a latched refusal would have cost.
kill -TERM $FAKEPID2 2>/dev/null; wait $FAKEPID2 2>/dev/null
wait_for "\[pinm0\] the rival stopped mastering this wire — the refusal is over" 25 || {
	echo "FAIL: the box stopped mastering and the refusal stood anyway"; tail -20 "$LOG"; exit 1; }
wait_for "\[pinm0\] segment up (master, pinned by REAC_ROLE_<segment>)" 15 || {
	echo "FAIL: the refusal ended and the pinned segment never came up"; tail -20 "$LOG"; exit 1; }
kill -TERM $SNIFF5 2>/dev/null; wait $SNIFF5 2>/dev/null
down_pair pinm0 mbox1

# ---- THE POSITIVE CONTROL FOR EVERY "IT NEVER SAID THAT" ABOVE. Two phases asserted the
# ABSENCE of the masterless-licence line (hear0 with a desk on it, and the yield's frame
# count). A grep that can never match reports absence exactly like a daemon that behaved,
# so the same string is required to be PRESENT for the wires that really were silent.
[ "$(grep -c "no REAC heard in .* taking it as MASTER" "$LOG")" -ge 2 ] || {
	echo "FAIL: the licence line never appeared for ANY wire, so every absence of it"
	echo "      asserted above was meaningless"; tail -30 "$LOG"; exit 1; }

kill -TERM $PID; wait $PID; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: clean SIGTERM exited $rc"; tail -5 "$LOG"; exit 1; }
echo "OK: heard, joined a desk as slave, kept through a flap, dropped past the hold,
    heard again, took a vacant wire as master, established with the box and put BOTH of
    its nodes on the graph, served a per-segment pin without a hunt, DROVE A PINNED WIRE
    ON LINK with a silent peer, DROVE an unpinned wire proven silent and established with
    the cold box that answered its stream, YIELDED that wire to a desk that turned up
    on it, JOINED A BOX MASTER on an unpinned wire at its own 8 ch and sent nothing back,
    and REFUSED the same box on a wire pinned master while PUBLISHING the door that says
    so -- then took that segment when the box stopped mastering it"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
