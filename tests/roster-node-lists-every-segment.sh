#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: the daemon publishes ONE port-less node that lists EVERY segment it runs —
# the empty ones included — and a drop-in in reac-pw.conf.d/ overrides the hand-written
# file on the real binary.
# (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
# 2026-09-16 third, §A and §B.)
#
# WHAT THIS EXISTS FOR. The amendment before this one took the per-segment zero-port door
# off the graph, and NAMED its own cost: the console's segment roster is a GRAPH SCAN, so
# an empty segment has no row at all — not one that says `probing`, none. The operator's
# other ruling the same day was that NOT AUTODETECTING IS AN ERROR, which is a sentence
# only a client that can SEE a probing segment can say. This node is that row.
#
# WHAT A UNIT TEST CANNOT SEE, and the reason for a whole-binary arm: tests/test_reac_roster.c
# pins the grammar and the delta, and every rule in it can be right while main.c derives the
# roster from nothing, publishes it nowhere, or builds a node per tick. This drives the REAL
# binary against REAL netdevs and reads the REAL graph.
#
#   A. ONE EMPTY WIRE AND ONE WITH A BOX. Exactly ONE node carries reac.roster=1, it has
#      NO ports, and it lists BOTH segments — the empty one `probing`/`none`/`0/0`, the
#      boxed one `established` with the width the daemon's own journal line names.
#   B. A DROP-IN BEATS THE HAND-WRITTEN FILE. reac-pw.conf says `role = master` for the
#      empty wire; reac-pw.conf.d/50-openmixer.conf says `role = tap`. The roster's row for
#      that segment reads role=tap AND names the drop-in as its source.
#   C. THE BOX LEAVES. The row goes back to `probing`/`none` and the node ID DOES NOT MOVE
#      — a state change is a property update, never a new node.
#
# PRESENCE BEFORE ABSENCE. The boxed segment is the positive control for everything: a
# roster that lists a segment it cannot describe, and a pw-dump filter that matches
# nothing, look identical to a working empty row.
#
# ISOLATION, both halves: a user+net+mount+pid namespace with its own veth, its own sysfs
# (sysfs does not follow a network namespace) and its own PipeWire on a private runtime
# dir, because `unshare -n` isolates the wire and not the graph. HOME is redirected at the
# daemon so the only conf it can read is the one written here — never the operator's.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw [/path/to/fake_box]}"
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

# THE ROSTER NODE, read off the graph and nowhere else: node -> client.id -> client ->
# application.process.id, so nothing else in this namespace can be mistaken for its work.
# Prints `roster <id> <ports> <key>=<value> ...` for every reac.roster* property, so every
# assertion below reads ONE line of real graph state.
roster_of() {
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
    # STR(), AND IT IS NOT A DETAIL: pw-dump renders a property whose value LOOKS numeric
    # as a JSON number, so `== "1"` on a property the daemon set to the string "1" is
    # false and the whole node reads as absent. Measured here, 2026-09-16.
    if str(p.get("reac.roster")) != "1": continue
    n=int(i.get("n_input_ports",0))+int(i.get("n_output_ports",0))
    kv=" ".join("%s=%s" % (k,v) for k,v in sorted(p.items()) if k.startswith("reac.roster"))
    print("roster", o["id"], p.get("node.name","?"), n, kv)
' "$1"
}
# EVERY OBJECT ON THE GRAPH THAT WEARS reac.roster, WHATEVER ITS TYPE — one line each,
# `<type> <id>`. The roster is a NODE; anything else carrying the same declaration is a
# decoy, and on 2026-09-16 exactly one existed: pw_filter_new_simple copies the properties
# it is given into the CONTEXT as well, so the CLIENT object wore node.name=reac-pw,
# media.class=Reac/Roster and reac.roster=1 with no roster on it at all. The operator ran
# `pw-cli info <that id>`, saw three properties and no roster, and filed a live defect
# against a daemon that was publishing correctly on the node next door.
wearers_of() {
	pw-dump | python3 -c '
import json,sys
pid=int(sys.argv[1])
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    t=o.get("type","?")
    p=o.get("info",{}).get("props",{})
    if str(p.get("reac.roster")) != "1": continue
    # OURS: a node by its client, a client by being one of ours.
    if t=="PipeWire:Interface:Client":
        if o["id"] not in mine: continue
    elif int(p.get("client.id",-1)) not in mine: continue
    print(t.rsplit(":",1)[-1], o["id"])
' "$1"
}
# The segment nodes, for the positive control: <node.name> <reac.segment>.
seg_nodes() {
	pw-dump | python3 -c '
import json,sys
pid=int(sys.argv[1]); seg=sys.argv[2]
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    if p.get("reac.segment")==seg: print(p.get("node.name","?"), seg)
' "$1" "$2"
}

ip link add rn0 type veth peer name prn0 || exit 90
ip link add rn1 type veth peer name prn1 || exit 90
# The peer end is another host, with its own sysfs for the same reason ours has one.
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set prn0 netns $NSPID || exit 90
ip link set prn1 netns $NSPID || exit 90
ip link set rn0 up; ip link set rn1 up
$in_peer ip link set prn0 up; $in_peer ip link set prn1 up

# THE TWO FILES, AND THEY DISAGREE. The hand-written one pins rn0 `master`; the console's
# drop-in pins it `tap`. Byte order puts the drop-in last, and last wins.
mkdir -p "$CONF/.config/reac-pw/reac-pw.conf.d"
printf '[segment rn0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"
printf '[segment rn0]\nrole = tap\n' > "$CONF/.config/reac-pw/reac-pw.conf.d/50-openmixer.conf"

HOME="$CONF" "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 4
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -12 "$LOG"; exit 91; }

# ---- THE BOX ARRIVES ON rn1 (the positive control for every reading below) -------------
$in_peer "$FAKE" prn1 90 >"$RT/box.log" 2>&1 &
FAKEPID=$!
for ((i = 0; i < 120; i++)); do
	[ "$(seg_nodes $PID rn1 | wc -l)" -ge 2 ] && break
	sleep 0.5
done
sleep 3
echo "control-segnodes $(seg_nodes $PID rn1 | wc -l)"
grep -a "autodetected .* -> reac-capture" "$LOG" | head -1 | sed 's/^/  control-log /'

# ---- A + B. THE ROSTER ITSELF ---------------------------------------------------------
echo "roster-count $(roster_of $PID | wc -l)"
roster_of $PID | sed 's/^/  A /'
echo "wearers-count $(wearers_of $PID | wc -l)"
wearers_of $PID | sed 's/^/  wearer /'
grep -a "roster is on the graph" "$LOG" | head -1 | sed 's/^/  rosterline /'

# ---- C. THE BOX GOES, AND THE NODE MUST NOT --------------------------------------------
kill -TERM $FAKEPID 2>/dev/null; kill -9 $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
for ((i = 0; i < 120; i++)); do
	roster_of $PID | grep -qa "rn1.*probing\|probing.*rn1" && break
	sleep 0.5
done
sleep 2
roster_of $PID | sed 's/^/  C /'
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
val() { echo "$OUT" | grep -a "^$1 " | head -1 | awk '{print $2}'; }
# The roster line of a phase, and one property out of it.
line_of() { echo "$OUT" | grep -a "^  $1 roster " | head -1; }
prop() {  # prop <phase> <key>
	line_of "$1" | tr ' ' '\n' | grep -a "^$2=" | head -1 | cut -d= -f2-
}
# Which group index carries segment <seg> in phase <phase>.
group_of() {
	line_of "$1" | tr ' ' '\n' | grep -a "^reac\.roster\.[0-9]*\.name=$2$" | head -1 \
		| sed 's/^reac\.roster\.\([0-9]*\)\.name=.*/\1/'
}

echo "$OUT" | grep -qa '^daemon-died' && fail "the daemon died at start"

# 0. THE CONTROL FIRST. If the box never brought its pair, this run has shown no presence
#    and nothing the roster says about `established` means anything.
[ "$(val control-segnodes)" = "2" ] \
	|| fail "the box on rn1 did not bring reac-capture + reac-playback (got $(val control-segnodes)) — no presence was shown, so no reading below is a measurement"
echo "$OUT" | grep -qa 'control-log.*autodetected .* -> reac-capture' \
	|| fail "the box never enrolled (no 'autodetected <model> -> reac-capture N in' line): this run has shown no presence, so no absence below is a measurement"

# A1. EXACTLY ONE ROSTER NODE, AND IT HAS NO PORTS.
[ "$(val roster-count)" = "1" ] \
	|| fail "the daemon publishes $(val roster-count) node(s) carrying reac.roster=1, expected exactly 1"
A=$(line_of A)
[ "$(echo "$A" | awk '{print $4}')" = "reac-pw" ] \
	|| fail "the roster node is not named reac-pw: $A"
[ "$(echo "$A" | awk '{print $5}')" = "0" ] \
	|| fail "the roster node has ports — it is a node about segments, never a door to one: $A"

# A1b. AND NOTHING ELSE ON THE GRAPH WEARS reac.roster. The declaration is how a client
#      FINDS the roster, so a second object carrying it is a decoy that answers `pw-cli
#      info` with three properties and no roster — which is precisely how this daemon was
#      reported broken while it was working (2026-09-16).
[ "$(val wearers-count)" = "1" ] \
	|| fail "$(val wearers-count) object(s) carry reac.roster=1 and only the NODE may: $(echo "$OUT" | grep -a '^  wearer ')"
echo "$OUT" | grep -qa '^  wearer Node ' \
	|| fail "the object carrying reac.roster=1 is not a Node: $(echo "$OUT" | grep -a '^  wearer ')"

# A1c. THE JOURNAL NAMES THE NODE ID, so the operator's next command reads the right
#      object. An id in a log line that points at something else is worse than no id.
rid=$(echo "$OUT" | grep -a '^  rosterline ' | sed 's/.*id \([0-9]*\).*/\1/')
[ -n "$rid" ] && [ "$rid" != "$(echo "$OUT" | grep -a '^  rosterline ')" ] \
	|| fail "the daemon announced the roster without naming its node id: $(echo "$OUT" | grep -a '^  rosterline ')"
[ "$rid" = "$(echo "$A" | awk '{print $3}')" ] \
	|| fail "the journal says the roster is node $rid and the graph says it is $(echo "$A" | awk '{print $3}')"

# A2. IT LISTS BOTH SEGMENTS: the empty one and the boxed one.
[ "$(prop A reac.roster.n)" = "2" ] \
	|| fail "the roster says it holds $(prop A reac.roster.n) segment(s), expected 2 (rn0 empty, rn1 boxed)"
g0=$(group_of A rn0); g1=$(group_of A rn1)
[ -n "$g0" ] || fail "the EMPTY segment rn0 is on no roster group — this is the row the whole amendment exists for: $A"
[ -n "$g1" ] || fail "the BOXED segment rn1 is on no roster group: $A"

# A3. THE EMPTY SEGMENT SAYS SO, in every field.
[ "$(prop A reac.roster.$g0.state)" = "tap" ] || [ "$(prop A reac.roster.$g0.state)" = "probing" ] \
	|| fail "rn0 has no box and reads state=$(prop A reac.roster.$g0.state)"
[ "$(prop A reac.roster.$g0.model)" = "none" ] \
	|| fail "an empty segment's model is '$(prop A reac.roster.$g0.model)', expected none"
[ "$(prop A reac.roster.$g0.width)" = "0/0" ] \
	|| fail "an empty segment's width is '$(prop A reac.roster.$g0.width)', expected 0/0"

# A4. THE BOXED SEGMENT CARRIES THE WIDTH THE DAEMON'S OWN JOURNAL NAMES. Cross-boundary:
#     the graph's property and the journal line are two publications of ONE fact, and a
#     roster that agrees with its own struct and nothing else proves nothing.
[ "$(prop A reac.roster.$g1.state)" = "established" ] \
	|| fail "rn1 has an enrolled box and reads state=$(prop A reac.roster.$g1.state)"
[ "$(prop A reac.roster.$g1.model)" != "none" ] \
	|| fail "rn1 has an enrolled box and its model reads none"
want=$(echo "$OUT" | grep -a 'control-log.*autodetected' | head -1 \
	| sed 's/.*reac-capture \([0-9]*\) in \/ reac-playback \([0-9]*\) out.*/\1\/\2/')
[ -n "$want" ] || fail "could not read the width out of the daemon's own autodetect line"
[ "$(prop A reac.roster.$g1.width)" = "$want" ] \
	|| fail "the roster says rn1 is $(prop A reac.roster.$g1.width) and the daemon's journal says $want — one of them is describing a rig that is not there"

# B. THE DROP-IN BEAT THE HAND-WRITTEN FILE, ON THE REAL BINARY, WITH PROVENANCE.
[ "$(prop A reac.roster.$g0.role)" = "tap" ] \
	|| fail "reac-pw.conf said master and reac-pw.conf.d/50-openmixer.conf said tap; the roster resolved role=$(prop A reac.roster.$g0.role) — the drop-in did not win"
case "$(prop A reac.roster.$g0.source)" in
	conf:reac-pw.conf.d/50-openmixer.conf) ;;
	*) fail "rn0's role came from the drop-in and its source reads '$(prop A reac.roster.$g0.source)' — an override nobody can trace to a file is the 2026-09-16 fault with one more file in it" ;;
esac

# C. THE BOX LEFT: the row follows it, and the NODE DOES NOT CHURN. The id is asserted, and
#    so is the roster's COMPLETENESS — measured 2026-09-16, a daemon that destroys and
#    rebuilds its node per publish gets the SAME id back from PipeWire and loses every key
#    it published before, so the completeness is the half of this arm that actually bites.
C=$(line_of C)
[ -n "$C" ] || fail "the roster node is GONE after the box left — it is the daemon's row and outlives every box"
[ "$(echo "$C" | awk '{print $3}')" = "$(echo "$A" | awk '{print $3}')" ] \
	|| fail "the roster node id moved from $(echo "$A" | awk '{print $3}') to $(echo "$C" | awk '{print $3}') — a state change is a property update, never a new node"
gc=$(group_of C rn1)
[ -n "$gc" ] || fail "rn1 left the roster when its box did — the segment is still there, it is just empty: $C"
[ "$(prop C reac.roster.$gc.state)" = "probing" ] \
	|| fail "the box left and rn1 still reads state=$(prop C reac.roster.$gc.state)"
[ "$(prop C reac.roster.$gc.model)" = "none" ] \
	|| fail "the box left and rn1 still names model=$(prop C reac.roster.$gc.model)"
[ "$(prop C reac.roster.$gc.width)" = "0/0" ] \
	|| fail "the box left and rn1 still claims width $(prop C reac.roster.$gc.width)"

echo "OK"
exit 0
