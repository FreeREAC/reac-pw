#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a segment's node COUNT on the graph follows its segment state, always.
# EXACTLY TWO nodes carry a segment's `reac.segment` while a box is recognised on it, and
# EXACTLY NONE while not -- never four, never one, across a box that arrives, LEAVES and
# ARRIVES AGAIN, and across a link-budget refusal loop on the same physical port.
# (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
# 2026-09-20 sec a; reac-pw#108.)
#
# THE FAULT, ON THE DESK 2026-09-20. `pw-dump` read FOUR reac-capture nodes for TWO
# segments: beside each established box's real pair stood a GHOST -- `reac.box-model=none`,
# `reac.box-width=0x0`, `reac.box.mac=none` -- carrying the SAME `reac.segment`. The
# console's segment scan is keyed by that property, so it read the ghost and dropped the
# established S-1608 entirely. Nothing in the journal announced a second birth: a pair with
# no owner is minted by a path that never logs one.
#
# WHAT THE EXISTING TESTS COULD NOT SEE, and why this is a count and not a presence.
# tests/no-box-no-node.sh asserts that a box brings a pair and that its departure takes the
# pair away -- and `>= 2` and `== 0` are both true of a segment carrying a real pair AND a
# ghost. A GHOST IS NOT AN ABSENCE, it is a surplus, and only an exact count can see one.
# This test therefore asserts `== 2` and `== 0` at every step and prints the nodes it
# counted, so a failure names the surplus instead of a number.
#
# PRESENCE BEFORE ABSENCE, and it is the structure of both arms: every `0` below is read by
# the same instrument that has just read a `2` on the same wire in the same run. An
# instrument that has never shown a presence reports a clean absence for a broken daemon and
# for a working one alike.
#
#   A. A SEGMENT COMES UP COLD, A BOX ARRIVES, LEAVES, AND ARRIVES AGAIN. 0 -> 2 -> 0 -> 2:
#      the cold wire has nothing, the box brings its pair, its departure takes the pair with
#      it, and the SECOND arrival brings back ONE pair and not a second beside a first that
#      was only forgotten. The re-arrival is the arm's whole point: a birth that is right
#      once says nothing about the birth after a teardown.
#   B. A PORT WHOSE BUDGET IS HELD, WITH THE OTHER VLAN REFUSED EVERY FEW SECONDS (#107's
#      own fixture, `--set REACPW_LINK_MBIT=100`). This is the ordering the desk saw the
#      ghost in -- a wire with an established box holding the port while a sibling VLAN on
#      the same physical port is refused by the link budget over and over. Every refusal is
#      a listener_open that runs and returns; not one of them may leave a node behind, on
#      EITHER segment.
#
# ISOLATION: a user+net+mount+pid namespace with its own veth, its own sysfs (sysfs does not
# follow a network namespace) and its own PipeWire on a private runtime dir, because
# `unshare -n` isolates the wire and not the graph. HOME is redirected at every daemon so
# the only conf any of them can read is the one written here -- never the operator's.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw [/path/to/fake_box]}"
# ABSOLUTE, ALWAYS: nsenter into a mount namespace starts at /, so a relative path runs one
# side of the wire and silently fails to start the other.
BIN=$(readlink -f "$BIN")
# NOT `${2:?}`: the meson `fake_box` option is empty by default, and a test that HARD-FAILS
# on an unset knob is a red about the machine wearing the clothes of a red about the code.
FAKE="${2:-}"
SKIP=77

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
FAKE=$(readlink -f "$FAKE")
for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

# ONE BODY, TWO ARMS, so the arm that is about a refusal cannot drift into testing a
# different daemon from the arm that is about a box.
run_arm() {   # run_arm <cold|budget>
unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" "$1" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; ARM="$3"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# EVERY NODE THIS DAEMON OWNS, ONE LINE EACH: `<node.name> <reac.segment> <box-model>`.
# Resolved node -> client.id -> client -> application.process.id, so nothing else in this
# namespace can be mistaken for its work. The box-model is printed because it is what NAMES
# a ghost when one is counted -- `none` beside a real pair is the desk's exact signature.
nodes_of() {
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
    if "reac.segment" not in p: continue     # the roster node is not a segment door
    print(p.get("node.name","?"), p.get("reac.segment"), p.get("reac.box-model","(unset)"))
' "$1"
}
seg_nodes() { nodes_of "$1" | awk -v s="$2" '$2 == s'; }
count()     { seg_nodes "$1" "$2" | wc -l; }
# Wait until a segment's node count REACHES a number, then report what it actually is. A
# count read once, at a moment of this script's choosing, is a race dressed as a measurement.
settle() {   # settle <pid> <segment> <want> <secs>
	local i
	for ((i = 0; i < $4 * 2; i++)); do
		[ "$(count "$1" "$2")" = "$3" ] && break
		sleep 0.5
	done
	sleep 1
}
wait_for() {   # wait_for <pattern> <secs>
	local i
	for ((i = 0; i < $2 * 5; i++)); do grep -qaE "$1" "$LOG" && return 0; sleep 0.2; done
	return 1
}

# ---- THE WIRE. One veth; the peer end is another host, with its own sysfs for the same
#      reason ours has one.
ip link add gs0 type veth peer name pgs0 || exit 90
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set pgs0 netns $NSPID || exit 90
ip link set gs0 up; $in_peer ip link set pgs0 up

if [ "$ARM" = "cold" ]; then
	# ---- ARM A: COLD, THEN A BOX, THEN A BOUNCE --------------------------------------
	HOME="$CONF" "$BIN" --live gs0 --tx gs0 --name gs0 --rate 96000 >"$LOG" 2>&1 &
	PID=$!
	sleep 4
	kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }
	echo "cold-count $(count $PID gs0)"

	$in_peer "$FAKE" pgs0 180 >"$RT/box.log" 2>&1 &
	FAKEPID=$!
	settle $PID gs0 2 60
	echo "box-count $(count $PID gs0)"
	seg_nodes $PID gs0 | sed 's/^/  box-node /'

	# THE BOX GOES. Its pair goes with it -- and this is also the control for the
	# re-arrival below: a count that never fell proves nothing about a count that rose.
	kill -TERM $FAKEPID 2>/dev/null; kill -9 $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
	settle $PID gs0 0 60
	echo "gone-count $(count $PID gs0)"
	seg_nodes $PID gs0 | sed 's/^/  gone-node /'

	# AND IT COMES BACK. The second birth is where a pair can be minted beside one that
	# was never destroyed; one arrival proves nothing about the second.
	$in_peer "$FAKE" pgs0 120 >"$RT/box2.log" 2>&1 &
	settle $PID gs0 2 60
	echo "again-count $(count $PID gs0)"
	seg_nodes $PID gs0 | sed 's/^/  again-node /'
	echo "ghosts $(seg_nodes $PID gs0 | awk '$3 == "none"' | wc -l)"
else
	# ---- ARM B: A HELD PORT AND A REFUSED SIBLING (#107's fixture) --------------------
	# Two VLANs on ONE physical port declared 100 Mbit/s: one 96 kHz master costs
	# 97 024 kbit/s of it, so the port carries exactly one and the other is refused every
	# few seconds. That is the ordering the ghost was seen in.
	for v in 11 13; do
		ip link add link gs0 name gs0.$v type vlan id $v || exit 90
		$in_peer ip link add link pgs0 name pgs0.$v type vlan id $v || exit 90
		$in_peer ip link set pgs0.$v up || exit 90
		ip link set gs0.$v up || exit 90
	done
	# The untagged parent would take the wire on silence like any other segment and hold
	# the very budget this arm is about; the rig's own remedy was the same file.
	mkdir -p "$CONF/.config/reac-pw"
	printf '[segment gs0]\nignore = yes\n' > "$CONF/.config/reac-pw/reac-pw.conf"

	HOME="$CONF" REAC_DEBUG=1 "$BIN" --set REACPW_LINK_MBIT=100 >"$LOG" 2>&1 &
	PID=$!
	sleep 2
	kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }
	grep -qa "S_KNOB_SET knob REACPW_LINK_MBIT=100" "$LOG" \
		&& echo "knob-announced yes" || echo "knob-announced no"

	# A BOX ON ONE OF THEM, this daemon's own `role = box` side -- the peer has to
	# DECLARE itself before anybody masters it, and libreac's fake_box is silent until a
	# master pushes a whole scene, which on a held port is a master that never starts.
	mkdir -p "$RT/boxes/.config/reac-pw"
	{ printf '[segment pgs0]\nignore = yes\n'
	  printf '[segment pgs0.13]\nignore = yes\n'
	  printf '[segment pgs0.11]\nrole = box\nmodel = s1608\n'
	} > "$RT/boxes/.config/reac-pw/reac-pw.conf"
	$in_peer env HOME="$RT/boxes" "$BIN" >"$RT/boxes.log" 2>&1 &
	sleep 2

	wait_for "S_SEGMENT_UP.*\[gs0\.11\]" 90 && echo "holder-up yes" || echo "holder-up no"
	settle $PID gs0.11 2 60
	wait_for "E_LINK_BUDGET.*\[gs0\.13\]" 60 && echo "refused yes" || echo "refused no"
	# LET THE REFUSAL LOOP RUN. The holder has a box of its own so it never yields, and
	# the empty sibling is re-served and re-refused every few seconds -- 690 times in 30
	# minutes on the desk the night the ghost appeared. Every one of those is a
	# listener_open that ran and returned, and the whole question is whether any of them
	# left a node behind, on either segment.
	sleep 20
	echo "refusals $(grep -ca 'E_LINK_BUDGET' "$LOG")"
	echo "box-count $(count $PID gs0.11)"
	seg_nodes $PID gs0.11 | sed 's/^/  box-node /'
	echo "empty-count $(count $PID gs0.13)"
	seg_nodes $PID gs0.13 | sed 's/^/  empty-node /'
	echo "parent-count $(count $PID gs0)"
	echo "ghosts $(nodes_of $PID | awk '$3 == "none"' | wc -l)"
	nodes_of $PID | sed 's/^/  any-node /'
fi
grep -a "E_ORPHAN_PAIR" "$LOG" | head -3 | sed 's/^/  orphan /'
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
}

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
val() { echo "$1" | grep -a "^ *$2 " | head -1 | awk '{print $2}'; }

# ---- ARM A ------------------------------------------------------------------------------
echo "=== arm: cold wire, box, box again ==="
A=$(run_arm cold); rc=$?
echo "$A" | sed 's/^/  /'
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$A" | grep -qa 'daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || { echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$A" | grep -qa '^SKIP:' && { echo "$A" | grep -a '^SKIP:'; exit $SKIP; }

# THE CONTROL FIRST: an instrument that never counted a pair cannot report a ghost or an
# absence, and every assertion after this one is about one or the other.
[ "$(val "$A" box-count)" = "2" ] \
	|| fail "a box enrolled and the segment carried $(val "$A" box-count) node(s), not 2 — this instrument has shown no correct presence, so nothing else in this arm is a measurement: $(echo "$A" | grep -a 'box-node')"
[ "$(val "$A" cold-count)" = "0" ] \
	|| fail "a cold wire with no box carried $(val "$A" cold-count) node(s) — a segment with nothing to carry is not on the graph at all"
[ "$(val "$A" gone-count)" = "0" ] \
	|| fail "the box left and $(val "$A" gone-count) node(s) outlived it: $(echo "$A" | grep -a 'gone-node')"
[ "$(val "$A" again-count)" = "2" ] \
	|| fail "the box came back and the segment carried $(val "$A" again-count) node(s), not 2 — a second pair beside one that was never destroyed is exactly #108: $(echo "$A" | grep -a 'again-node')"
[ "$(val "$A" ghosts)" = "0" ] \
	|| fail "$(val "$A" ghosts) node(s) on an established segment carry reac.box-model=none — the desk's own ghost signature: $(echo "$A" | grep -a 'again-node')"

# ---- ARM B ------------------------------------------------------------------------------
echo "=== arm: a held port, a refused sibling ==="
B=$(run_arm budget); rc=$?
echo "$B" | sed 's/^/  /'
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$B" | grep -qa 'daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || { echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$B" | grep -qa '^SKIP:' && { echo "$B" | grep -a '^SKIP:'; exit $SKIP; }

[ "$(val "$B" knob-announced)" = "yes" ] \
	|| fail "REACPW_LINK_MBIT=100 was never announced — the port was not capped, nothing was ever refused, and this arm is not about #108's ordering at all"
[ "$(val "$B" refused)" = "yes" ] \
	|| fail "the empty sibling was never refused on the budget — the ordering this arm exists for did not happen"
[ "$(val "$B" refusals)" -ge 2 ] 2>/dev/null \
	|| fail "only $(val "$B" refusals) budget refusal(s): the refusal LOOP is what the desk saw a ghost under, and one refusal is not a loop"
[ "$(val "$B" holder-up)" = "yes" ] \
	|| fail "the segment with the box never came up, so this arm counted no presence and its zeros mean nothing"
[ "$(val "$B" box-count)" = "2" ] \
	|| fail "the established segment carried $(val "$B" box-count) node(s), not 2, while its sibling was being refused: $(echo "$B" | grep -a 'box-node')"
[ "$(val "$B" empty-count)" = "0" ] \
	|| fail "the REFUSED sibling carried $(val "$B" empty-count) node(s) after $(val "$B" refusals) refusals — an open that refused left something behind: $(echo "$B" | grep -a 'empty-node')"
[ "$(val "$B" ghosts)" = "0" ] \
	|| fail "$(val "$B" ghosts) node(s) carry reac.box-model=none on this graph — the desk's own ghost signature: $(echo "$B" | grep -a 'any-node')"

[ $FAIL -eq 0 ] || exit 1
echo "OK"
exit 0
