#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a daemon started with NOTHING finds a segment by hearing it, takes the
# end of the pairing the wire leaves open, PUTS BOTH OF THAT SEGMENT'S NODES ON THE GRAPH,
# keeps the segment through a link flap, drops it when the link stays down, hears it again
# afterwards, obeys a per-segment pin without a hunt, and -- since 0.5.3 -- HEARS 802.1Q
# TAGS ON A TRUNK, makes the sub-interface each VLAN needs, serves them as ordinary
# segments, and takes away only the netdevs it made.
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
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
# THE PID NAMESPACE IS WHAT MAKES "NOTHING OF OURS OUTLIVES THIS SCRIPT" TRUE UNDER A
# KILL. The EXIT trap below covers every path the script takes ITSELF, and it does; what
# it cannot cover is the script being SIGKILLed -- a meson timeout, a Ctrl-C, an agent
# stopped mid-run. Five peer daemons from interrupted runs were found reparented to
# systemd on 2026-09-09, each still holding the nested peer network namespace open, two
# hours after the run that started them. Inside a PID namespace pid 1 is this shell, and
# the kernel SIGKILLs every remaining process in the namespace the instant it dies, so an
# interrupted run cleans itself up whether or not the trap ever ran.
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
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

# THE SAME WAIT, ANCHORED. Untagged journal lines -- the slave engine's STATE transitions
# are one segment's news printed without its name -- match an EARLIER phase's line and the
# wait returns instantly on somebody else's success. Every phase after the first that waits
# on an untagged line has to say where its own log begins.
LINE0() { echo $(( $(wc -l < "$LOG") + 1 )); }
wait_for_since() {   # wait_for_since <first-line> <pattern> <secs>
	local floor="$1" pat="$2" secs="$3" i
	for ((i = 0; i < secs * 5; i++)); do
		tail -n "+$floor" "$LOG" | grep -q "$pat" && return 0
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
    # THE ROSTER NODE IS NOT A SEGMENT DOOR and is deliberately skipped here. Since
    # 2026-09-16 (spec amendment third, section B) this daemon always publishes ONE node for
    # ITSELF -- `reac-pw`, no ports, reac.roster=1 -- listing every segment it runs,
    # probing ones included. This probe is about the doors a SEGMENT has, and counting that
    # row as one of them would read as a ghost door on every empty wire.
    if p.get("reac.roster") is not None: continue
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
# Prints: state|rival.kind|refusal|master.mac|segment|node.description|box.mac|
#         box-model|box-width|link-state|cfg.role.state|pace.source
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
                    p.get("node.description","(none)"),
                    p.get("reac.box.mac","(none)"),
                    p.get("reac.box-model","(none)"),
                    p.get("reac.box-width","(none)"),
                    p.get("reac.link-state","(none)"),
                    p.get("reac.cfg.role.state","(none)"),
                    p.get("reac.pace.source","(none)")]))
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
mkpair venue0 vbox0 || exit 90
# THE TRUNK. trunk0 carries two VLANs and neither netdev exists on our side; trunk1
# carries one whose sub-interface is pre-created below, so adoption and creation are told
# apart in the same run.
mkpair trunk0 tbox0 || exit 90
mkpair trunk1 tbox1 || exit 90

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
$in_peer "$BIN" --live desk0 --tx desk0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" --name desk \
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
grep -q "\[hear0\] listening — role auto (autodetected)" "$LOG" || { echo "FAIL: hear0 not sniffed, or its role did not resolve to auto"; cat "$LOG"; exit 1; }
grep -qE "S_SEGMENT_HEARD.*\[hear0\]" "$LOG" || { echo "FAIL: master on the peer never heard"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
grep -qE "S_SEGMENT_UP.*\[hear0\]" "$LOG" || { echo "FAIL: heard but not served"; cat "$LOG"; exit 1; }
# AND IT WAS NEVER TAKEN. A wire with a master on it is not silent, so the masterless
# licence must never have been granted here. This is an ABSENCE claim and it gets its
# positive control at the end of the file, where the same string is REQUIRED to have
# appeared for cold0 -- a grep that cannot match is indistinguishable from a daemon that
# behaved.
if grep -q "\[hear0\] no REAC heard in" "$LOG"; then
	echo "FAIL: a desk was streaming on hear0 and the daemon called the wire silent"
	cat "$LOG"; exit 1
fi
# AND ON THE RIGHT END OF THE PAIRING. A desk masters this wire, so the daemon DEFERS to
# it: served as a TAP, which transmits nothing at all. Nothing was configured to say so;
# the verdict came from the frames.
#
# IT WAS A COURTING SLAVE UNTIL 2026-09-14, and the operator's ruling on libreac's
# bounded-ungranted-courtship spec (option C) is why it is not. Beside a real M-200 our
# courting slave kept that desk's own S-1608 from enrolling for 180 s, and once the desk
# had granted our slave it blocked the box outright — four trials, 2026-09-12. `recorder`
# (reac-pw.conf [segment <name>] role=slave) is still available and still courts; nothing RESOLVES to it
# beside a desk any more.
grep -q "\[hear0\] a desk masters this segment" "$LOG" || {
	echo "FAIL: a desk was mastering the wire and the hunt did not say so"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] segment up (tap, deferring to the master it heard)" "$LOG" || {
	echo "FAIL: served, but not as the tap the ruling calls for beside a desk"; cat "$LOG"; exit 1; }
# AND THE SENTENCE IS THE ACT. The hunt's own line said "joining it as SLAVE" over a
# segment that went on to be served as a tap; one predicate answers both now.
grep -q "\[hear0\] a desk masters this segment .* — TAPPING it" "$LOG" || {
	echo "FAIL: the journal announced a role the daemon did not take"; cat "$LOG"; exit 1; }
# NOT ONE FRAME FROM US on a desk's wire is the whole point of the role; the absence is
# measured with its own positive control in tests/tap-sends-nothing.sh, and asserted here
# as the JOURNAL's claim that no TX socket was opened at all.
grep -q "\[hear0\] TAP up at .* NOTHING is transmitted on this segment" "$LOG" || {
	echo "FAIL: served as a tap and never said it opened no TX side"; cat "$LOG"; exit 1; }
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
grep -qE "S_SEGMENT_DROPPED.*\[hear0\]" "$LOG" || { echo "FAIL: no drop after the hold"; cat "$LOG"; exit 1; }
peer ip link set desk0 up; sleep 5
[ "$(grep -c "\[hear0\] listening — role auto (autodetected)" "$LOG")" -ge 2 ] || {
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
# is slower, the same verdict is reached on PROVEN SILENCE and the line says so instead
# (2026-09-16: the sentence used to claim a box either way). The grep below matches both,
# because it asks for the verdict and not for the reason. The wire is then taken on the
# licence. The phase's real assertion is the next one -- the segment comes up as MASTER -- and everything after it.
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

# ---- A PIN IS SERVED WITHOUT A HUNT. `reac-pw.conf` is an answer about this
# wire -- a setting, not a guess -- so it waits only for the wire to BE a segment. The box
# stays where it is; only the conf changes, and the segment is bounced so the sniffer
# re-reads it.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment hear0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null
peer ip link set desk0 down; sleep 4.5
peer ip link set desk0 up;   sleep 1
$in_peer "$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] reac-pw.conf \[segment hear0\] role pins this segment as MASTER — driving on link" 15 || {
	echo "FAIL: the per-segment pin was not honoured on the first classifying frame"
	tail -20 "$LOG"; exit 1; }
wait_for "\[hear0\] segment up (master, pinned by reac-pw.conf)" 10 || {
	echo "FAIL: served, but not reported as pinned"; tail -20 "$LOG"; exit 1; }
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null

# ---- A PINNED MASTER DRIVES ON LINK, WITH A SILENT PEER. The 2026-09-08 22:10 defect
# exactly: nothing on the far end says anything, ever, until a master announces to it, so
# a pinned master that waits for a classifying frame waits forever. Here the peer is a
# bare veth end -- no daemon, no box, not one byte -- and the assertion is that OUR
# announce is on the wire within 2 s of LINK. The peer's capture is what proves it left
# this host; a journal line would only prove we decided to.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment pin0]\nrole = master\n' >> "$CONF/.config/reac-pw/reac-pw.conf"
up_pair pin0 pbox0
$in_peer python3 "$RT/sniff.py" pbox0 "$RT/pin0.cnt" & SNIFF1=$!
wait_for "\[pin0\] listening — role master (reac-pw.conf): driving on link" 10 || {
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
wait_for "\[pin0\] segment up (master, pinned by reac-pw.conf)" 15 || {
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
wait_for "\[cold0\] listening — role auto (autodetected)" 10 || {
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
$in_peer "$BIN" --live kdesk1 --tx kdesk1 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" --name kdesk \
       --src-mac 00:40:ab:de:5c:02 >"$PEER" 2>&1 &
KDESKPID=$!
wait_for "\[cold1\] a desk masters this segment .* yielding the master role" 25 || {
	echo "FAIL: a desk took the wire we were driving and we did not yield"
	tail -25 "$LOG"; tail -5 "$PEER"; exit 1; }
# AND IT COMES BACK AS A TAP, not as a courting slave (courtship ruling 2026-09-14,
# option C). A yield to a desk is the same deferral the first phase makes on a wire a desk
# already held; the two must not be two different answers to one fact.
wait_for "\[cold1\] segment up (tap, deferring to the master it heard)" 15 || {
	echo "FAIL: yielded, but never came back up as the tap the ruling calls for"
	tail -25 "$LOG"; exit 1; }
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

# ---- THE VENUE CASE, END TO END (0.5.4): the console drives the boxes, the desk is
# switched on afterwards, and later switched off again. This is the wire's whole life and
# no phase above covers it: cold1 yields a wire that had NOTHING on it, so nothing was
# lost by yielding and nothing came back. Here a box is GRANTED and carrying audio when
# the desk arrives -- the case 0.5.0 refused to yield ("taken on evidence") and the
# operator ruled on 2026-09-09 -- and the desk then goes away, which the daemon had no
# path back from at all: it stayed slaved to a wire nobody was driving.
#
# BOTH PEERS SIT ON THE SAME PEER END. A veth pair is point-to-point, so the box and the
# desk are two daemons in the peer namespace sourcing from two different addresses onto
# vbox0 -- which is exactly what a stagebox and a desk on one venue switch look like from
# our side of the wire.
VBOXMAC=00:40:ab:c4:80:51
VDESKMAC=00:40:ab:de:5c:03
$in_peer python3 "$RT/sniff.py" vbox0 "$RT/venue.cnt" & SNIFFV=$!
up_pair venue0 vbox0
wait_for "\[venue0\] listening — role auto (autodetected)" 10 || {
	echo "FAIL: venue0 never came up as an unpinned sniffer"; tail -20 "$LOG"; exit 1; }
# EITHER ROUTE TO THE WIRE IS CORRECT HERE, and which one runs is a race nobody needs to
# win -- since 0.5.4 both keep the sniffer, which is the whole point of the release. The
# box floods broadcast FILLER on its PHY-up, and a FILLER from a peer no control frame has
# proved yet is deliberately NOT a sighting (reac_disco's peer lock), so a box that has not
# joined yet leaves the wire looking empty: measured here, the licence route ran.
for ((i = 0; i < 60; i++)); do
	grep -qE "\[venue0\] (no master heard in|no REAC heard in .* taking it as MASTER)" "$LOG" && break
	sleep 0.2
done
grep -qE "\[venue0\] (no master heard in|no REAC heard in .* taking it as MASTER)" "$LOG" || {
	echo "FAIL: a masterless wire with a box on it was taken by neither route"
	tail -25 "$LOG"; tail -5 "$RT/venue-box.log"; exit 1; }
wait_for "\[venue0\] segment up (master, chosen by hearing the wire)" 15 || {
	echo "FAIL: venue0 was taken and never served"; tail -20 "$LOG"; exit 1; }
# THE BOX IS POWERED ON A WIRE THAT IS ALREADY BEING DRIVEN -- the rig's own case, and the
# only order in which a stagebox enrols: it leaves BOOT for ANNOUNCE on ITS OWN PHY-up
# edge and cold-connects then, so a box already up when we start driving floods once and
# is never heard from again (measured here: rx_box_frames climbing with rx_joins=0).
$in_peer "$BIN" --live vbox0 --tx vbox0 --role slave --box-channels 16 --name vbox \
       --src-mac $VBOXMAC >"$RT/venue-box.log" 2>&1 &
VBOXPID=$!
# AND THE BOX IS OURS: granted, enrolled, established. "The box is no longer granted by
# us" below is an ABSENCE claim, so it is worth nothing until this presence is on record.
VLINE0=$(LINE0)
wait_for "\[venue0\] autodetected" 25 || {
	echo "FAIL: driving venue0 never enrolled the box that answered"
	tail -25 "$LOG"; tail -5 "$RT/venue-box.log"; exit 1; }
# ESTABLISHED, NOT MERELY RECOGNIZED, AND THAT IS NOT A DETAIL. `autodetected` is printed
# when the box's CONFIG-ANNOUNCE is read, which happens in GRANTING -- before the grant
# has been accepted and the pair is streaming. This phase's last assertion (further down)
# requires the box to have ESTABLISHED at some point, so gating on the weaker line let the
# desk be switched on INSIDE the handshake: the grant was re-sent to a box that was still
# cold-connecting, we yielded mid-grant, and the phase failed at its own end over a
# condition it had never actually waited for. The bar is the master's own FSM, anchored to
# this phase so an earlier segment's ESTABLISHED cannot answer for it.
wait_for_since "$VLINE0" "reac-master: .* -> ESTABLISHED" 25 || {
	echo "FAIL: venue0 recognized the box and the pair never established"
	tail -30 "$LOG"; tail -5 "$RT/venue-box.log"; exit 1; }
sleep 1
daemon_nodes $PID | grep -q "^reac-playback.venue0 " || {
	echo "FAIL: venue0 is master and has no playback door -- the graph probe would then"
	echo "      report its absence below whatever the daemon did. Nodes:"; daemon_nodes $PID; exit 1; }
# THE PACE THE DOOR PUBLISHES IS THE PACE THE DAEMON IS KEEPING (0.5.4). Two publications
# of one fact, and the defect was that they disagreed: on the rig 2026-09-08/09 the journal
# read `locked to graph clock (api.alsa.0)` and this row read `free-run`, because the door
# built its arbitration with the constant. The row is asserted against the journal rather
# than against a value this namespace happens to produce -- what a private PipeWire with no
# hardware offers as a reference is not the rig's business, but AGREEING is.
# READ TOGETHER, AND GIVEN TIME TO AGREE. The pace CHANGES while a segment runs — the DLL
# locks to the box's counter slope some seconds into the session — so reading the two once
# asks whether they happened to be in step at one instant. What the key is for is that the
# row CONVERGES on what the daemon is keeping, so both are re-read until they do. Without
# that convergence the row never catches up at all: measured in the 1.0.3 RPM's %check,
# the door read `free-run` beside `locked to box counter slope (S-1608)` in the same
# journal, because the props publish was guarded on the SIGHTING SEQUENCE and a pace
# change is not a sighting. 15 s is ~75 publish ticks and cannot pass by waiting.
for ((i = 0; i < 30; i++)); do
	VPACE=$(fld "$(daemon_node_props $PID reac-playback.venue0)" 12)
	CLK=$(grep "reac-clock:" "$LOG" | tail -1)
	case "$CLK" in
		*"locked to graph clock"*)       W=graph-ref ;;
		*"locked to NIC/external PHC"*)  W=phc ;;
		*"locked to box counter slope"*) W=box-slope ;;
		*"locked to master cadence"*)    W=foreign-master ;;
		*)                               W=free-run ;;
	esac
	[ "$VPACE" = "$W" ] && break
	sleep 0.5
done
# The transcript is the daemon's other publication of the same fact, and the last line of
# it is the state it is in. Only "locked to X" names a reference: "acquiring" is a claim
# about the future and "holdover" is a frozen period nothing is steering, and both run on
# CLOCK_MONOTONIC at that instant. Measured in this namespace, where a private PipeWire
# with no hardware offers no reference at all: 8 reac-clock lines, none of them a lock, so
# free-run is the true answer here -- and the ASSERTION is the agreement, not the value.
CLKLINE=$(grep "reac-clock:" "$LOG" | tail -1)
[ -n "$CLKLINE" ] || {
	echo "FAIL: the daemon published no clock transcript at all, so there is nothing to"
	echo "      check the door's pace against"; exit 1; }
case "$CLKLINE" in
	*"locked to graph clock"*)       WANT=graph-ref ;;
	*"locked to NIC/external PHC"*)  WANT=phc ;;
	*"locked to box counter slope"*) WANT=box-slope ;;
	*"locked to master cadence"*)    WANT=foreign-master ;;
	*)                               WANT=free-run ;;
esac
[ "$VPACE" = "$WANT" ] || {
	echo "FAIL: the daemon publishes two answers about one pace -- the door says '$VPACE'"
	echo "      and its own transcript says '$WANT':"
	echo "      $CLKLINE"
	echo "      (this is the 2026-09-08 rig defect: 'locked to graph clock (api.alsa.0)'"
	echo "      in the journal beside 'free-run' in the row)"; exit 1; }
# THE DESK IS SWITCHED ON, on the wire we are mastering with a box enrolled on it.
$in_peer "$BIN" --live vbox0 --tx vbox0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" --name vdesk \
       --src-mac $VDESKMAC >"$RT/venue-desk.log" 2>&1 &
VDESKPID=$!
# WITHIN ONE ANNOUNCE CADENCE. A desk announces itself once a second; the hunt reads the
# table on the 200 ms poll, so the yield is a second's business and not a window's. The
# bar is 5 s because a loaded machine may miss the first announce, and it still cannot
# pass by waiting the 3 s hunt window out.
wait_for "\[venue0\] a desk masters this segment .* yielding the master role" 5 || {
	echo "FAIL: a desk took a wire we had won by hearing a box, and we did not yield"
	tail -30 "$LOG"; tail -5 "$RT/venue-desk.log"; exit 1; }
# AS A TAP, since the courtship ruling of 2026-09-14 (option C): a desk's own boxes are
# what a courting slave of ours blocks, and this segment has a box on it.
wait_for "\[venue0\] segment up (tap, deferring to the master it heard)" 15 || {
	echo "FAIL: yielded, but never came back up as the desk's tap"; tail -30 "$LOG"; exit 1; }
# AND THE BOX IS NO LONGER GRANTED BY US. Two independent facts, because one of them
# alone is a shape: the master DOOR is off the graph (a slave publishes no
# reac-playback.<segment> -- reac_source_node.c's "what is not here is not an omission"),
# and we have stopped BROADCASTING, which is what granting and driving a box IS.
sleep 2
if daemon_nodes $PID | grep -q "^reac-playback.venue0 "; then
	echo "FAIL: we yielded venue0 to a desk and the master door is still on the graph"
	daemon_nodes $PID; exit 1
fi
daemon_nodes $PID | grep -q "^reac-capture.venue0 " || {
	echo "FAIL: the segment lost its capture node in the yield -- the absence above is"
	echo "      then a missing segment, not a surrendered master role"; daemon_nodes $PID; exit 1; }
# AND THE PACE IS THE DESK'S, PUBLISHED AS SUCH. Whatever we would have disciplined to is
# not what this wire is running on any more, and the row says which.
YPACE=$(fld "$(daemon_node_props $PID reac-capture.venue0)" 12)
[ "$YPACE" = "foreign-master" ] || {
	echo "FAIL: we joined a desk that times this wire and the segment publishes pace"
	echo "      '$YPACE'"; daemon_node_props $PID reac-capture.venue0; exit 1; }
# OUR ADDRESS IS READ, NOT GUESSED. cold1 takes the busiest source on the wire as ours,
# which is true there because nothing else was driving; here a desk is, so the phase asks
# the kernel for venue0's own MAC -- the address every emitting role of ours sources from
# (reac_mac.h) -- and the capture keys on it.
# OVER NETLINK, NOT SYSFS: /sys is not remounted in this namespace, so
# /sys/class/net still lists the HOST's interfaces and venue0 is simply not there.
OURV=$(ip -o link show venue0 | awk '{for (i = 1; i <= NF; i++) if ($i == "link/ether") print $(i+1)}' | tr -d ':')
[ ${#OURV} -eq 12 ] || {
	echo "FAIL: could not read venue0's own address (got '$OURV')"; ip -o link show venue0; exit 1; }
[ "$(seen x "$RT/venue.cnt" "$OURV")" -gt 0 ] || {
	echo "FAIL: nothing of ours ever reached vbox0"; cat "$RT/venue.cnt"; tail -20 "$LOG"; exit 1; }
BEFOREV=$(seen x "$RT/venue.cnt" "$OURV-b"); DESKB=$(other "$RT/venue.cnt" "$OURV")
sleep 4
AFTERV=$(seen x "$RT/venue.cnt" "$OURV-b");  DESKA=$(other "$RT/venue.cnt" "$OURV")
[ "$DESKA" -gt "$((DESKB + 1000))" ] || {
	echo "FAIL: the peer capture is not receiving ($DESKB -> $DESKA frames from everyone"
	echo "      but us), so it cannot testify that we stopped driving"; cat "$RT/venue.cnt"; exit 1; }
[ "$((AFTERV - BEFOREV))" -lt 500 ] || {
	echo "FAIL: we yielded venue0 and BROADCAST $((AFTERV - BEFOREV)) frames in 4 s anyway"
	echo "      -- the box is still being granted by us, over a desk"; exit 1; }
# A FRESH EAR FOR THE LAST MEASUREMENT, AND THIS IS NOT TIDINESS. The counter above has
# been reading a wire carrying two masters and a box at 8000 frames a second each; a
# python capture cannot drain that in real time, so its counts are minutes behind the
# cable by now -- measured, as a segment that had just come up MASTER and whose freshly
# read broadcast total had not moved in two seconds because the file was still describing
# the phase before. A capture that lags reports a silent daemon exactly like a silent one.
kill -TERM $SNIFFV 2>/dev/null; wait $SNIFFV 2>/dev/null
$in_peer python3 "$RT/sniff.py" vbox0 "$RT/venue2.cnt" & SNIFFV=$!
# THE DESK IS SWITCHED OFF AT THE END OF THE NIGHT. The wire must come back to us: a
# segment slaved to nobody is a dead segment, and the console's boxes are still on it.
kill -TERM $VDESKPID 2>/dev/null; wait $VDESKPID 2>/dev/null
# AFTER THE HOLD, and not before it: the bar is the discovery table's own withdrawal
# window (REAC_DISCO_STALE_NS, 5 s), which is what "the rival is really gone" means
# everywhere else in this daemon. 20 s covers it on a loaded machine.
wait_for "\[venue0\] the desk stopped mastering this wire" 20 || {
	echo "FAIL: the desk went away and the wire never came back to us -- the segment is"
	echo "      slaved to something that is not there"; tail -30 "$LOG"; exit 1; }
wait_for "\[venue0\] segment up (master, chosen by hearing the wire)" 15 || {
	echo "FAIL: the reclaim was announced and the segment never came up as master"
	tail -30 "$LOG"; exit 1; }
# AND WE ARE DRIVING IT AGAIN -- the whole point of taking the wire back. The journal
# line is a claim; the peer's own capture is the measurement, and it is the same bar cold0
# uses for "a master, not a knock": thousands of broadcasts a second, not half of one.
RB0=$(seen x "$RT/venue2.cnt" "$OURV-b"); RB1=$RB0
for ((i = 0; i < 60; i++)); do
	sleep 0.5
	RB1=$(seen x "$RT/venue2.cnt" "$OURV-b")
	[ "$((RB1 - RB0))" -gt 500 ] && break
done
[ "$((RB1 - RB0))" -gt 500 ] || {
	echo "FAIL: venue0 came back as master and only $((RB1 - RB0)) frames reached the peer"
	echo "      in 30 s -- the segment is master in the journal and silent on the cable"
	cat "$RT/venue2.cnt"; tail -30 "$LOG"; exit 1; }
# THE NODES COME BACK WITH THE BOX, NOT WITH THE ROLE, and that is asserted nowhere here
# because it is not this release's to promise: a master in autodetect sizes
# reac-capture/reac-playback from the box it RECOGNIZES, so a retaken segment whose box
# has not cold-connected again publishes no node until it does. Measured on this run --
# the graph held nothing for venue0 at this point -- and stated rather than forgotten.
# WHAT THE RETAKE DOES NOT DO, AND MUST NOT BE READ AS DOING: it does not re-enrol the
# box by itself. A REAC stagebox leaves BOOT for ANNOUNCE on ITS OWN PHY-up edge and on
# nothing else (reac_linkmon.h, #95), and this peer proves it -- its transcript ends at
# ESTABLISHED and stays there through the desk's whole visit, so it has no reason to
# cold-connect to anybody. What we owe it is a master that is DRIVING when it does, which
# is what the frame count above measures; the master engine probes for exactly that.
grep -q "reac_slave: .*STATE .* -> ESTABLISHED" "$RT/venue-box.log" || {
	echo "FAIL: this phase's box never established at all, so nothing above is about a"
	echo "      granted box"; tail -20 "$RT/venue-box.log"; exit 1; }
kill -TERM $SNIFFV 2>/dev/null; wait $SNIFFV 2>/dev/null
kill -TERM $VBOXPID 2>/dev/null; wait $VBOXPID 2>/dev/null
down_pair venue0 vbox0

# ---- A BOX MASTERS AN UNPINNED WIRE: WE JOIN IT, AT ITS OWN WIDTH (0.5.1).
# The 2026-09-09 rig proof in miniature, with the ruling applied: an S-0808 on M is not a
# hazard to refuse, it is a clock to follow. The peer is the fake box master -- broadcast
# box geometry, one master-only record a second, no handshake, which is what a stagebox on
# M really does (reac-protocol/wire-format.md) -- and it is started BEFORE the link comes
# up, so the wire carries a master from the first instant of carrier and the masterless
# licence is not in the race at all.
BOXMAC=00:40:ab:c4:08:bc
BOXM_FLOOR=$(LINE0)
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
wait_for "\[boxm0\] segment up (slave" 20 || {
	echo "FAIL: joined in the journal, but the segment never came up"
	tail -20 "$LOG"; exit 1; }
# AND IT ENROLS WITH IT, WHICH TAKES A FLOOD AND A HANDSHAKE (0.5.6). The lamp follows the
# engine now, not the RX, so the assertions below wait for the pairing rather than for the
# first decoded frame.
wait_for_since "$BOXM_FLOOR" "reac_slave: .*STATE .*-> ESTABLISHED" 60 || {
	echo "FAIL: the box master granted nothing, or we never enrolled with it"
	grep "reac_slave: .*STATE" "$LOG" | tail -6; tail -5 "$RT/boxm.log"; exit 1; }
# THE JOB: the box's channels are on the graph, sized by what the box announced -- 8, not
# a 40-slot fabric with 32 rows of silence -- and the node says whose clock they are on.
sleep 1.5
BP=$(daemon_node_props $PID reac-capture.boxm0)
[ -n "$BP" ] || { echo "FAIL: no reac-capture.boxm0 on the graph after joining a box master"
	daemon_nodes $PID; tail -20 "$LOG"; exit 1; }
# SIZED TO THE BOX AND NAMED AFTER IT (0.5.6-9): the identity keys were always published,
# but the DESCRIPTION is what a console shows the operator, and a joined box read the generic
# "REAC 8ch capture" where a served one names the model.
case "$(fld "$BP" 6)" in
  "S-0808 (8 in / 8 out)"*"8 ch"*) : ;;
  *) echo "FAIL: the joined segment should name the box and its own geometry: $BP"; exit 1 ;;
esac
[ "$(fld "$BP" 1)" = "foreign" ] || { echo "FAIL: master.state is not foreign: $BP"; exit 1; }
[ "$(fld "$BP" 2)" = "box" ] || { echo "FAIL: master.rival.kind is not box: $BP"; exit 1; }
[ "$(fld "$BP" 3)" = "none" ] || {
	echo "FAIL: we JOINED it, so master.refusal must be none: $BP"; exit 1; }
[ "$(fld "$BP" 4)" = "$BOXMAC" ] || {
	echo "FAIL: the joined node names a master other than the box: $BP"; exit 1; }
[ "$(fld "$BP" 5)" = "boxm0" ] || { echo "FAIL: the joined node names another segment: $BP"; exit 1; }
# AND IT IS THE SAME BOX IT IS WHEN WE MASTER IT (0.5.2). A console keys a stagebox off
# reac.box.mac / reac.box-model / reac.box-width / reac.link-state; a join that published
# none of them left the rig showing a segment and no device at all, with the box's eight
# inputs unpatchable (2026-09-09 05:45). The model is IMPLIED BY THE WIDTH here -- a box on
# M broadcasts upstream geometry and no config-announce, so there is nothing to identify it
# from -- and 8 in is the S-0808 row of the fixed matrix, exactly.
[ "$(fld "$BP" 7)" = "$BOXMAC" ] || {
	echo "FAIL: the joined box has no address of its own (reac.box.mac), so a console"
	echo "      cannot fold it into the box it knows by MAC: $BP"; exit 1; }
[ "$(fld "$BP" 8)" = "s0808" ] || {
	echo "FAIL: an 8-ch box master is the S-0808 row of the matrix and must say so"
	echo "      (reac.box-model): $BP"; exit 1; }
[ "$(fld "$BP" 9)" = "8x8" ] || {
	echo "FAIL: reac.box-width must be the recognised model's own geometry: $BP"; exit 1; }
# THE LAMP IS THE PAIRING (0.5.6, operator ruling): established means ENROLLED — granted,
# unicasting and heartbeating — not merely heard. The rig published `established` off the
# RX's own evidence while the box's front lamp sat unlocked ("S-0808 is not enrolled but omx
# sees it available").
[ "$(fld "$BP" 10)" = "established" ] || {
	echo "FAIL: we are enrolled with the box master, so reac.link-state must be"
	echo "      established: $BP"
	grep -E "reac_slave: STATE|\[boxm0\]" "$LOG" | tail -12; tail -3 "$RT/boxm.log"; exit 1; }
# AND THE ROLE IS APPLIED. role_reestablish_pending means the engine asked for is not the
# one performing; the receive-only join IS the slave role performed -- there is no
# enrolment to wait for on a peer that grants nothing -- and the rig published pending over
# a segment that was up and streaming.
[ "$(fld "$BP" 11)" = "applied" ] || {
	echo "FAIL: a locked receive-only join must publish reac.cfg.role.state=applied: $BP"
	exit 1; }
# THE AUDIO IS ARRIVING, and it is the box's own frames that carry it. The counter is the
# feeder's own (REAC_DEBUG telemetry): ok= counts frames DECODED INTO THE RING, which is
# where reac_source_node's process() reads the port planes from. The samples themselves are
# read by value in tests/test_reac_box_master_audio.c -- not here, because no session
# manager runs in this namespace, no node in this graph materialises a port, and nothing
# schedules the process() that would fill one (measured: this same telemetry reads
# active_ch=0 peak=0.000000 for the whole run while ok= climbs into the tens of thousands).
# THE LINE IS KEYED BY SEGMENT, not by the source address: a box that MASTERS the wire
# broadcasts, and a broadcast never locks the feeder's peer, so src reads all-zero here.
# The telemetry prints its first line on the first frame (ok=1) and then every 2 s, so a
# read taken right after the segment came up is a read of the FIRST line -- which is why
# this waits for a line that clears the bar instead of asserting on whatever is there.
RXOK=0
for _i in $(seq 30); do
	RXOK=$(sed -n 's/^reac_rx: \[boxm0\] ok=\([0-9]*\) .*/\1/p' "$LOG" | tail -1)
	[ -n "$RXOK" ] && [ "$RXOK" -gt 100 ] && break
	sleep 0.4
done
[ -n "$RXOK" ] && [ "$RXOK" -gt 100 ] || {
	echo "FAIL: the feeder decoded no audio from the box master $BOXMAC (ok='$RXOK'), so"
	echo "      the 8 ports it published carry nothing"; grep "reac_rx: \[boxm0\]" "$LOG" | tail -3
	exit 1; }
# AND WE SPEAK ON THAT WIRE (0.5.6). Until 0.5.4 this phase asserted the opposite -- a
# receive-only join that put NOTHING on the wire. 0.5.5 sent a desk's downstream at the box,
# which the ground-truth capture retired; what a stagebox on M actually grants is a SLAVE
# speaking its own geometry, so what leaves us here is the announce flood and then the
# unicast upstream. Asserted at the coarse grain this file works at, on the peer's own
# capture and against the same live control; the recipe frame by frame -- the flood width,
# the announce-before-burst ordering, the tone in the upstream and the heartbeat -- is
# tests/box-master-slave-join.sh.
sleep 1
OURS_B=$(other "$RT/boxm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXFR=$(seen x "$RT/boxm0.cnt" "$(echo $BOXMAC | tr -d :)")
[ "$BOXFR" -gt 500 ] || {
	echo "FAIL: the peer capture has only $BOXFR frames from the box master itself, so what"
	echo "      it says about our own frames cannot be trusted either"
	cat "$RT/boxm0.cnt"; exit 1; }
[ "$OURS_B" -gt 500 ] || {
	echo "FAIL: a joined box master received $OURS_B frames from us. Since 0.5.5 this wire"
	echo "      carries our downstream -- the box's outputs come from it"
	cat "$RT/boxm0.cnt"; exit 1; }
# AND ITS OUTPUTS ARE ON THE GRAPH. A segment whose box we can only listen to is what
# 0.5.1 shipped; the playback node is how an operator routes to it.
daemon_nodes $PID | grep -q "^reac-playback.boxm0 " || {
	echo "FAIL: a joined box master published no reac-playback.boxm0, so its outputs are"
	echo "      unroutable"; daemon_nodes $PID; exit 1; }
kill -TERM $SNIFF4 2>/dev/null; wait $SNIFF4 2>/dev/null
kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
down_pair boxm0 mbox0

# ---- THE SAME BOX ON A WIRE PINNED MASTER: JOINED TOO (operator, 2026-09-16).
# "We set the daemons to enroll any box, master or slave." The pin says which end we want
# and a stagebox on M has already answered; the daemon settles it by taking the box's audio
# rather than by out-shouting it or serving nothing. It REFUSED until 2026-09-16 and the rig
# measured the cost: a pinned enp131s0 with an S-1608 on M published a door, moved no audio,
# and the operator read "not detected". What the switch position still costs is the head-amp,
# and saying so is the console's job.
#
# THE PIN STILL DRIVES FIRST, and that is not a defect to test around. A pinned master is
# served on LINK with no frame waited for, because a cold stagebox in slave mode transmits
# nothing until a master announces to it (the 2026-09-08 outage). So the daemon cannot know
# a box is mastering this wire until it has listened, and its own engine -- which classifies
# every frame on that wire -- is what tells it, about a second later. The phase asserts the
# END STATE: the segment comes down as a master and goes back up as that box's slave, at the
# box's own width, carrying the box's identity.
printf '[segment pinm0]\nrole = master\n' >> "$CONF/.config/reac-pw/reac-pw.conf"
$in_peer "$FAKE" mbox1 "$BOXMAC" 8 2000 >"$RT/boxm1.log" 2>&1 &
FAKEPID2=$!
sleep 0.5
up_pair pinm0 mbox1
$in_peer python3 "$RT/sniff.py" mbox1 "$RT/pinm0.cnt" & SNIFF5=$!
wait_for "\[pinm0\] reac-pw.conf \[segment pinm0\] role pins this segment MASTER and .* JOINING it as its slave" 20 || {
	echo "FAIL: a pinned master beside a box on M did not join it"
	grep -n "pinm0" "$LOG" | tail -20; tail -3 "$RT/boxm1.log"
	echo "--- conf:"; cat "$CONF/.config/reac-pw/reac-pw.env"; exit 1; }
wait_for "\[pinm0\] segment up (slave" 20 || {
	echo "FAIL: joined in the journal, but the segment never came back up as a slave"
	tail -20 "$LOG"; exit 1; }
sleep 1.5
RP=$(daemon_node_props $PID reac-capture.pinm0)
[ -n "$RP" ] || { echo "FAIL: the joined wire published NO capture node at all"
	daemon_nodes $PID; tail -20 "$LOG"; exit 1; }
[ "$(fld "$RP" 1)" = "foreign" ] || {
	echo "FAIL: the joined node does not say master.state=foreign: $RP"; exit 1; }
[ "$(fld "$RP" 2)" = "box" ] || { echo "FAIL: the joined node does not say rival.kind=box: $RP"; exit 1; }
# THE LINE THIS PHASE EXISTS FOR since 2026-09-16: nothing is refused any more.
[ "$(fld "$RP" 3)" = "none" ] || {
	echo "FAIL: a pinned wire beside a box on M must JOIN it, not refuse: $RP"; exit 1; }
[ "$(fld "$RP" 4)" = "$BOXMAC" ] || {
	echo "FAIL: the joined node names a master other than the rival $BOXMAC: $RP"; exit 1; }
[ "$(fld "$RP" 5)" = "pinm0" ] || { echo "FAIL: the joined node names another segment: $RP"; exit 1; }
# AND IT IS THE SAME BOX A CONSOLE ALREADY KNOWS, by its own address and model -- the 0.5.2
# rule, which a pin must not change.
[ "$(fld "$RP" 7)" = "$BOXMAC" ] || {
	echo "FAIL: the joined box has no address of its own (reac.box.mac): $RP"; exit 1; }
[ "$(fld "$RP" 8)" = "s0808" ] || {
	echo "FAIL: an 8-ch box master is the S-0808 row of the matrix and must say so: $RP"; exit 1; }
# AND IT IS AN ENGINE, NOT A DOOR: a refusal published a capture node with nothing behind
# it, while a joined slave carries the box BOTH ways -- its inputs in, our returns out -- so
# the playback side is present and sized to the box. That presence is the difference between
# this phase's answer and the one it replaced.
daemon_nodes $PID | grep -q "^reac-playback.pinm0 " || {
	echo "FAIL: we joined this box as its slave and published no reac-playback node, so"
	echo "      nothing can be returned to it -- that is a refusal door wearing a join's name"
	daemon_nodes $PID; exit 1; }
# AND WE ARE TALKING TO IT, not at it: a slave returns its inputs upstream, so OUR frames on
# this wire must be RISING. Measured as a delta over a window that begins after the join,
# with the box's own frames on the same capture over the same window as the positive control:
# a capture that has died reports absence exactly like a daemon that has stopped.
BEFORE_P=$(other "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXB=$(seen x "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
sleep 3
AFTER_P=$(other "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
BOXA=$(seen x "$RT/pinm0.cnt" "$(echo $BOXMAC | tr -d :)")
[ "$BOXA" -gt "$((BOXB + 1000))" ] || {
	echo "FAIL: the box's own frames went $BOXB -> $BOXA on this capture, so it is not"
	echo "      receiving and cannot testify about what we sent"; cat "$RT/pinm0.cnt"; exit 1; }
[ "$((AFTER_P - BEFORE_P))" -gt 50 ] || {
	echo "FAIL: we joined this box as its slave and put only $((AFTER_P - BEFORE_P)) frames"
	echo "      on the wire in 3 s -- a slave that returns nothing has not enrolled"
	cat "$RT/pinm0.cnt"; exit 1; }
# ---- AND MOVING THE PIN AT RUNTIME CHANGES NOTHING HERE ANY MORE (0.5.6-8, amended
# 2026-09-16). The rig's own sequence was: a wire pinned MASTER with a stagebox on M is a
# refusal door, the operator changes the pin to `auto`, and what must follow is the BOX-MASTER
# JOIN. What followed instead was the DESK-slave engine -- "rx stream = master downstream
# (40 ch)" -- because the in-place role swap carried the listener's old configuration across
# and `join_box_master` is the HUNT's verdict, which the swap never re-ran.
#
# Since "enrol any box, master or slave" both pins reach the SAME end state, so what this
# phase now guards is that the assertion does not knock the segment OFF it: the box-master
# join must survive a role write, in both directions, and the desk-slave engine must never
# appear on a wire carrying a box's own geometry.
PINM_FLOOR=$(LINE0)
sed -i 's/^role = master$/role = auto/' "$CONF/.config/reac-pw/reac-pw.conf"
# The role assertion reaches the daemon the way the console sends it: a write on the door's
# own reac.cfg.role param. The conf above is what a re-open reads on the way back up.
# EVERY DOOR THIS SEGMENT HAS, because which node carries the cfg door depends on which
# engine is open: a master reads it on reac-playback, a lone slave on reac-capture, and a
# JOINED box master publishes both. Aiming at one name by hand is how a write lands nowhere
# and the phase reads the silence as a verdict.
role_write() {
	local want=$1 ids
	ids=$(pw-dump | python3 -c "
import json,sys
for o in json.load(sys.stdin):
    if o.get('type')!='PipeWire:Interface:Node': continue
    n=o['info']['props'].get('node.name','')
    if n.endswith('.pinm0') and n.startswith('reac-'): print(o['id'])")
	[ -n "$ids" ] || { echo "FAIL: no door node to assert a role on"; exit 1; }
	for id in $ids; do
		pw-cli set-param "$id" Props "{ params = [ \"reac.cfg.role\", $want ] }" >/dev/null 2>&1
	done
}
role_write 1
sleep 3
RP=$(daemon_node_props $PID reac-capture.pinm0)
[ "$(fld "$RP" 2)" = "box" ] && [ "$(fld "$RP" 3)" = "none" ] || {
	echo "FAIL: a role write knocked the box-master join off the wire: $RP"
	tail -n "+$PINM_FLOOR" "$LOG" | grep pinm0 | tail -12; exit 1; }
if tail -n "+$PINM_FLOOR" "$LOG" | grep -q "\[pinm0\] .*rx stream = master downstream ($FACT_MAX_CHANNELS ch)"; then
	echo "FAIL: the segment opened the DESK-slave engine -- the wire carries a box's own"
	echo "      geometry, not a desk's 40-channel downstream"; exit 1
fi
echo "OK: a runtime role write leaves the box-master join standing"
# THE REVERSE DIRECTION IS NOT ASSERTED HERE, and the reason is the ruling rather than a gap
# nobody noticed: since "enrol any box, master or slave" both pins reach the SAME end state on
# this wire, so writing the role back to master asks the daemon for the engine it is already
# going to run. There is no swap to observe and a phase that waited for one would be waiting
# for a transition the design no longer has. Where a role write DOES swap engines — a wire
# with no box mastering it — the swap's own re-classification is exercised by the desk phases
# above. OWED: a swap-path arm for the joined-box case, once a box can be made to change mode
# under a running daemon (a fake box master cannot: the switch is read at boot).
sed -i 's/^role = auto$/role = master/' "$CONF/.config/reac-pw/reac-pw.conf"

# ---- AND THE JOIN IS NOT A LATCH EITHER. The switch is moved to slave: the box stops
# mastering, its sighting ages out, and the wire the operator pinned is driven after all --
# without a restart, which is what a latched verdict would have cost.
kill -TERM $FAKEPID2 2>/dev/null; wait $FAKEPID2 2>/dev/null
wait_for "\[pinm0\] segment up (master, pinned by reac-pw.conf)" 40 || {
	echo "FAIL: the box stopped mastering and the pinned segment never took the wire"
	grep pinm0 "$LOG" | tail -20; exit 1; }
kill -TERM $SNIFF5 2>/dev/null; wait $SNIFF5 2>/dev/null
down_pair pinm0 mbox1

# ---- A TRUNK: TWO VLANS ON ONE WIRE, TWO SEGMENTS, AND THE NETDEVS ARE OURS (0.5.3).
# Nothing in src/ read a VLAN tag before this release, and the reason a trunk is hard is
# measured rather than argued: the parent's own 0x8819 socket receives every tagged frame
# with the tag GONE (openmixer's tools/probe-vlan-8819.py, fact B), so a daemon that
# enumerated interfaces naively would run one listener on the parent for two boxes and
# answer untagged onto the native VLAN. Only an ETH_P_ALL tap reading PACKET_AUXDATA can
# tell which VLAN a frame came from.
#
# The peer is another host with two VLAN sub-interfaces of its own and a fake box master
# broadcasting on each -- ONE veth, two VIDs -- which is the four-boxes-one-trunk rig in
# miniature. What has to happen: the daemon hears vid 11 and vid 12 on trunk0, CREATES
# trunk0.11 and trunk0.12 with nothing typed, serves each as an ordinary segment with a
# box on it, and refuses to be a segment on the parent itself.
peer ip link add link tbox0 name tbox0.11 type vlan id 11 || exit 90
peer ip link add link tbox0 name tbox0.12 type vlan id 12 || exit 90
# AND THE COLD ONE (ruling 2026-09-22). VLAN 14 carries NO REAC at all, ever: its only
# traffic is a switch-shaped frame on an ethertype that is not ours, which is what a trunk
# port carries for every VLAN whatever the boxes are doing. That is the whole case the
# ruling is about -- a stagebox is a slave and says nothing until a master speaks, and the
# master cannot speak until the netdev exists, so before this the VLAN was reachable only
# by a hand-written declaration.
peer ip link add link tbox0 name tbox0.14 type vlan id 14 || exit 90
cat > "$RT/tagnoise.py" <<'PYEOF2'
# The switch's own voice: an LLDP-shaped frame out of a VLAN sub-interface, so the KERNEL
# inserts the tag (nothing here writes an 802.1Q header) and it reaches the trunk's parent
# tagged and unmistakably not REAC. Slow on purpose: one frame is the whole fact.
import socket, sys, time
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x88cc))
s.bind((sys.argv[1], 0))
f = b"\x01\x80\xc2\x00\x00\x0e" + b"\x02\x00\x00\x0e\x00\x14" + b"\x88\xcc" + bytes(50)
while True:
    try:
        s.send(f)
    except OSError:
        pass
    time.sleep(0.25)
PYEOF2
# AND THE ADOPTION CASE, set up before the daemon can ever hear vid 13: a host that keeps
# its own network configuration pre-creates the sub-interface, and the daemon must take it
# as it finds it. It carries no mint alias of ours, so it is not ours to remove.
ip link add link trunk1 name trunk1.13 type vlan id 13 || exit 90
peer ip link add link tbox1 name tbox1.13 type vlan id 13 || exit 90

TBOX11=00:40:ab:c4:11:11
TBOX12=00:40:ab:c4:12:12
TBOX13=00:40:ab:c4:13:13
$in_peer "$FAKE" tbox0.11 "$TBOX11" 8 2000 >"$RT/tb11.log" 2>&1 & TFAKE1=$!
$in_peer "$FAKE" tbox0.12 "$TBOX12" 16 2000 >"$RT/tb12.log" 2>&1 & TFAKE2=$!
$in_peer "$FAKE" tbox1.13 "$TBOX13" 8 2000 >"$RT/tb13.log" 2>&1 & TFAKE3=$!
sleep 0.5
up_pair trunk0 tbox0
up_pair trunk1 tbox1
peer ip link set tbox0.11 up; peer ip link set tbox0.12 up; peer ip link set tbox1.13 up
peer ip link set tbox0.14 up
$in_peer python3 "$RT/tagnoise.py" tbox0.14 >"$RT/tag14.log" 2>&1 & TAGNOISE=$!
ip link set trunk1.13 up

# THE TAG IS HEARD, AND IT IS HEARD PER VID. This is the assertion that could not have
# been made at all before this release.
wait_for "\[trunk0\] tagged REAC heard — vid 11" 20 || {
	echo "FAIL: tagged REAC on vid 11 was never heard on the trunk parent"
	tail -30 "$LOG"; tail -3 "$RT/tb11.log"; exit 1; }
wait_for "\[trunk0\] tagged REAC heard — vid 12" 20 || {
	echo "FAIL: vid 11 was heard and vid 12 was not, so the detector is not per-VLAN"
	tail -30 "$LOG"; tail -3 "$RT/tb12.log"; exit 1; }
# THE NETDEVS ARE MADE, with nothing typed, and the journal says which is which.
wait_for "\[trunk0\] vid 11: created trunk0.11 (marked reac-pw:minted)" 20 || {
	echo "FAIL: vid 11 was heard and trunk0.11 was never created"; tail -30 "$LOG"; exit 1; }
wait_for "\[trunk0\] vid 12: created trunk0.12 (marked reac-pw:minted)" 20 || {
	echo "FAIL: vid 12 was heard and trunk0.12 was never created"; tail -30 "$LOG"; exit 1; }
# ---- THE COLD VLAN: HEARD FROM A FRAME THAT IS NOT REAC, AND SERVED (ruling 2026-09-22).
# Nothing has EVER spoken REAC on vid 14 in this run, and nothing will. All the daemon has
# is the switch-shaped frame arriving tagged on the parent -- which is all it has on a cold
# rig, where every box is a slave waiting for a master that cannot exist until its netdev
# does. The frame COUNT in the line is the control: a detector that saw nothing cannot
# print one.
wait_for "\[trunk0\] tagged VLAN heard — vid 14" 20 || {
	echo "FAIL: vid 14 carried tagged frames that were not REAC and the VLAN was never"
	echo "      heard. On a cold trunk that is the only evidence there is, so the segment"
	echo "      would exist only if someone hand-wrote it into reac-pw.conf."
	tail -30 "$LOG"; tail -3 "$RT/tag14.log"; exit 1; }
grep -a "\[trunk0\] tagged VLAN heard — vid 14" "$LOG" | grep -qaE '\(([1-9][0-9]*) frame' || {
	echo "FAIL: vid 14 was named with NO frame count — a detector that saw nothing cannot"
	echo "      have named it"; grep -a "vid 14" "$LOG" | head -3; exit 1; }
wait_for "\[trunk0\] vid 14: created trunk0.14 (marked reac-pw:minted)" 20 || {
	echo "FAIL: vid 14 was heard and no sub-interface was made for it, so a box arriving"
	echo "      on that VLAN has nowhere to be heard"; tail -30 "$LOG"; exit 1; }
# AND THE JOURNAL DOES NOT INVENT WHAT IT HEARD. "tagged REAC heard" over an LLDP frame
# would be the #102 report again with the evidence fabricated rather than misattributed.
# The claim is an ABSENCE and its positive control is the two REAC sightings asserted
# above: the same grep, over the same log, matches vid 11 and vid 12.
grep -qa "tagged REAC heard — vid 14" "$LOG" && {
	echo "FAIL: nothing REAC has ever been on vid 14 and the daemon said it heard some"
	grep -a "vid 14" "$LOG" | head -5; exit 1; }
wait_for "\[trunk1\] vid 13: adopted trunk1.13 — the host made it" 20 || {
	echo "FAIL: a pre-created sub-interface must be ADOPTED, not re-created"
	# THE FIRST THING TO LOOK AT IS WHETHER THE PARENT WAS WATCHED AT ALL. Both bounds
	# are 8 (taps and table rows), and until 0.5.4 a dropped segment gave neither back:
	# past the eighth interface of a run this phase failed here, with the daemon behaving
	# perfectly and simply unable to see the tag.
	grep -E "no room for a topology tap|topology table is full" "$LOG" | tail -5
	tail -30 "$LOG"; exit 1; }
# AND ADOPTION IS NOT A RE-CREATION. The one line that would prove the opposite must be
# absent, and its positive control is the two creates asserted above: the same daemon said
# "created" twice in this run, so a grep that cannot match is not what is being read here.
grep -q "\[trunk1\] vid 13: created" "$LOG" && {
	echo "FAIL: trunk1.13 already existed and the daemon created it anyway"
	tail -30 "$LOG"; exit 1; }
# THE PARENT IS NOT A SEGMENT. Fact B in one line: the untagged copies of both boxes'
# frames arrive on trunk0, and a daemon that served them would put a master over two
# VLANs' boxes at once.
wait_for "\[trunk0\] this parent carries tagged REAC, so it is not itself a segment" 20 || {
	echo "FAIL: the trunk parent was never named as a trunk"; tail -30 "$LOG"; exit 1; }

# THE JOB: BOTH VLANS ARE SERVED AS ORDINARY SEGMENTS, each following its own box's clock,
# each with its own node on the graph at its own width. Two boxes, one cable.
wait_for "\[trunk0.11\] segment up (slave, enrolling with the box that masters it, chosen by hearing the wire)" 25 || {
	echo "FAIL: trunk0.11 was created and never served as a segment"; tail -30 "$LOG"; exit 1; }
wait_for "\[trunk0.12\] segment up (slave, enrolling with the box that masters it, chosen by hearing the wire)" 25 || {
	echo "FAIL: trunk0.12 was created and never served as a segment"; tail -30 "$LOG"; exit 1; }
wait_for "\[trunk1.13\] segment up (slave, enrolling with the box that masters it, chosen by hearing the wire)" 25 || {
	echo "FAIL: the adopted trunk1.13 was not served like any other interface"
	tail -30 "$LOG"; exit 1; }
# AND NOTHING WAS STACKED ON A STACK. A sub-interface has no VLANs of its own: the frame
# reaching it has already had its tag consumed, and a daemon that tapped one would read
# that same VID again and mint `<parent>.<vid>.<vid>`. Measured here before 0.5.4 -- inside
# this namespace /sys is the HOST's, so the daemon's `lower_*` test answers "not stacked"
# for every netdev in the run, and it minted trunk1.13.13 while the real segment probed at
# a wire nobody was on. The daemon no longer needs sysfs to refuse it.
if ip -o link | awk '{print $2}' | tr -d ':' | grep -qE '\.[0-9]+\.[0-9]+'; then
	echo "FAIL: a VLAN was created on a VLAN"; ip -o link | awk '{print $2}' | tr -d ':'
	grep -E "created|adopted" "$LOG" | tail -10; exit 1
fi
sleep 1.5
T11=$(daemon_node_props $PID reac-capture.trunk0.11)
T12=$(daemon_node_props $PID reac-capture.trunk0.12)
T13=$(daemon_node_props $PID reac-capture.trunk1.13)
[ -n "$T11" ] && [ -n "$T12" ] && [ -n "$T13" ] || {
	echo "FAIL: a trunk's VLANs must reach the graph as ordinary segments; got"
	echo "      11='$T11' 12='$T12' 13='$T13'"; daemon_nodes $PID; tail -30 "$LOG"; exit 1; }
# THE TWO VLANS ARE TWO DIFFERENT BOXES, and nothing has crossed between them: each node
# names its own segment, its own box address and its own width. A single listener on the
# parent -- the fault fact B invites -- would have produced one node, not two, and could
# not have told 8 channels from 16.
[ "$(fld "$T11" 5)" = "trunk0.11" ] || { echo "FAIL: vid 11's node names segment '$(fld "$T11" 5)'"; exit 1; }
[ "$(fld "$T12" 5)" = "trunk0.12" ] || { echo "FAIL: vid 12's node names segment '$(fld "$T12" 5)'"; exit 1; }
[ "$(fld "$T11" 7)" = "$TBOX11" ] || { echo "FAIL: vid 11's node carries box.mac '$(fld "$T11" 7)', not $TBOX11"; exit 1; }
[ "$(fld "$T12" 7)" = "$TBOX12" ] || { echo "FAIL: vid 12's node carries box.mac '$(fld "$T12" 7)', not $TBOX12"; exit 1; }
# Each VLAN's door is sized to ITS OWN box and named after it (0.5.6-9), which is also how
# two segments on one cable are told apart at a glance.
case "$(fld "$T11" 6)" in
  "S-0808 (8 in / 8 out)"*"8 ch"*) : ;;
  *) echo "FAIL: vid 11's box is 8 ch and its node reads '$(fld "$T11" 6)'"; exit 1 ;;
esac
case "$(fld "$T12" 6)" in
  "S-1608 (16 in / 8 out)"*"16 ch"*) : ;;
  *) echo "FAIL: vid 12's box is 16 ch and its node reads '$(fld "$T12" 6)' -- two VLANs"
     echo "      served through one listener would read the same width twice"; exit 1 ;;
esac
[ "$(fld "$T13" 7)" = "$TBOX13" ] || { echo "FAIL: the adopted VLAN's node carries box.mac '$(fld "$T13" 7)'"; exit 1; }
# AND THE AUDIO IS ARRIVING ON BOTH, decoded into the ring the ports read from -- the
# feeder's own counter, per segment. A tag we could see and a stream we could not decode
# would be a topology detector with no product behind it.
for SEG in trunk0.11 trunk0.12; do
	OK=0
	for _i in $(seq 30); do
		OK=$(sed -n "s/^reac_rx: \[$SEG\] ok=\([0-9]*\) .*/\1/p" "$LOG" | tail -1)
		[ -n "$OK" ] && [ "$OK" -gt 100 ] && break
		sleep 0.4
	done
	[ -n "$OK" ] && [ "$OK" -gt 100 ] || {
		echo "FAIL: $SEG is up and decoded no audio (ok='$OK'), so its ports carry nothing"
		# WHAT THE COUNTER WAS DOING, not just what it ended at. ok=1 with a live wire has
		# two very different causes and the line already carries both: `other=` climbing is
		# the gate refusing a source it locked onto, and a SEQUENCE of ok=1 lines is a
		# segment being served again and again. A failure that cannot tell them apart
		# costs a whole rig session to reproduce.
		echo "      the feeder's own telemetry, last 6 lines:"
		grep "reac_rx: \[$SEG\]" "$LOG" | tail -6 | sed "s/^/        /"
		echo "      serves=$(grep -c "\[$SEG\] segment up" "$LOG") drops=$(grep -c "\[$SEG\] segment dropped" "$LOG")"
		grep "reac_rx: \[$SEG\]" "$LOG" | tail -3
		grep -E "trunk" "$LOG" | tail -40; ip -o link show | cut -d: -f2
		tail -3 "$RT/tb11.log" "$RT/tb12.log" "$RT/tb13.log"; exit 1; }
done
# AND THE PARENT WAS NEVER SERVED, not even for one poll. This is the fault fact B
# invites and the reason the tap is the authority on a tapped parent: the sniffer cannot
# tell a tagged frame from an untagged one, so a parent served on the sniffer's word alone
# would carry a master over two VLANs' boxes until the tap caught up. The claim is an
# ABSENCE, and its positive control is the three "segment up" lines asserted above for the
# VLANs themselves: the same grep, over the same log, matches those.
grep -qE "\[trunk0\] segment up|\[trunk1\] segment up" "$LOG" && {
	echo "FAIL: a trunk parent was served as a segment -- it receives every VLAN's frames"
	echo "      with the tag gone, so that is one master over two boxes"
	grep -E "trunk" "$LOG" | tail -30; exit 1; }
# AND THE PLAIN UNTAGGED NIC IS NOT A SPECIAL CASE: every phase above this one ran on an
# untagged wire in this same run and was served by the same code.

# ---- THE POSITIVE CONTROL FOR EVERY "IT NEVER SAID THAT" ABOVE. Two phases asserted the
# ABSENCE of the masterless-licence line (hear0 with a desk on it, and the yield's frame
# count). A grep that can never match reports absence exactly like a daemon that behaved,
# so the same string is required to be PRESENT for the wires that really were silent.
[ "$(grep -c "no REAC heard in .* taking it as MASTER" "$LOG")" -ge 2 ] || {
	echo "FAIL: the licence line never appeared for ANY wire, so every absence of it"
	echo "      asserted above was meaningless"; tail -30 "$LOG"; exit 1; }

kill -TERM $PID; wait $PID; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: clean SIGTERM exited $rc"; tail -5 "$LOG"; exit 1; }

# ---- WHAT WE MINTED, WE TOOK AWAY; WHAT WE ADOPTED IS STILL THERE. The netdev lifecycle
# of the trunk spec's 4d, read off the kernel after the daemon is gone rather than off its
# own journal. The two claims are asserted in ONE sweep of `ip link`, so the surviving
# trunk1.13 is the positive control for the absence of the other two: a sweep that could
# not see any of the three would report the same emptiness as a daemon that cleaned up.
LINKS=$(ip -o link show | awk -F': ' '{print $2}' | cut -d@ -f1)
echo "$LINKS" | grep -qx "trunk1.13" || {
	echo "FAIL: trunk1.13 was ADOPTED -- the host made it and the daemon must leave it"
	echo "      behind. It is gone, and with it the control for the two claims below."
	echo "$LINKS"; exit 1; }
echo "$LINKS" | grep -qx "trunk0.11" && {
	echo "FAIL: the daemon created trunk0.11 and left it behind on a clean exit"
	echo "$LINKS"; tail -10 "$LOG"; exit 1; }
echo "$LINKS" | grep -qx "trunk0.12" && {
	echo "FAIL: the daemon created trunk0.12 and left it behind on a clean exit"
	echo "$LINKS"; tail -10 "$LOG"; exit 1; }
echo "$LINKS" | grep -qx "trunk0.14" && {
	echo "FAIL: the daemon created trunk0.14 for a VLAN it only HEARD and left it behind"
	echo "      on a clean exit. A VID discovered from a switch's own traffic is minted on"
	echo "      the same terms as any other: what we made, we take away."
	echo "$LINKS"; tail -10 "$LOG"; exit 1; }
grep -q "\[trunk0.11\] removed — we created it" "$LOG" || {
	echo "FAIL: trunk0.11 is gone and the daemon never said it removed it"; tail -10 "$LOG"; exit 1; }
grep -q "\[trunk1.13\] left alone — the host made it" "$LOG" || {
	echo "FAIL: the adopted netdev survived, and the journal does not say it was left"
	echo "      alone deliberately -- a survival nobody claimed is a leak that got lucky"
	tail -10 "$LOG"; exit 1; }
kill -TERM $TFAKE1 $TFAKE2 $TFAKE3 $TAGNOISE 2>/dev/null; wait $TFAKE1 $TFAKE2 $TFAKE3 $TAGNOISE 2>/dev/null
echo "OK: heard, joined a desk as slave, kept through a flap, dropped past the hold,
    heard again, took a vacant wire as master, established with the box and put BOTH of
    its nodes on the graph, served a per-segment pin without a hunt, DROVE A PINNED WIRE
    ON LINK with a silent peer, DROVE an unpinned wire proven silent and established with
    the cold box that answered its stream, YIELDED that wire to a desk that turned up
    on it, ENROLLED WITH A BOX MASTER on an unpinned wire, its way, at its own 8 ch,
    and REFUSED the same box on a wire pinned master while PUBLISHING the door that says
    so -- then took that segment when the box stopped mastering it; HEARD TWO 802.1Q VIDS
    ON ONE VETH, created a sub-interface for each with nothing typed, served both as
    segments at their own widths with audio decoding on both, refused to be a segment on
    the trunk parent, ADOPTED a pre-created sub-interface, and on exit removed what it
    minted and left what it adopted"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
