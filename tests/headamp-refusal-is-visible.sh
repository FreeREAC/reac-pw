#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A REFUSED HEAD-AMP WRITE IS VISIBLE TO A CLIENT THAT DID NOT WRITE IT.
#
# The ruling (docs/design/specs/2026-09-14-headamp-as-node-params.md §3a, RULED by the
# operator 2026-09-14): the head-amp door publishes `reac.headamp.state`,
# `reac.headamp.refused`, `reac.headamp.asserted` and `reac.headamp.sens.max` on the same
# master sink node that accepts the `reac.headamp.<ch>.<param>` control keys.
#
# THE DEFECT IT EXISTS AGAINST: a cell written to a segment with no preamps at all
# returned NOTHING. `pw-cli set-param` exited 0, the parse dropped the cell, and the caller
# had a successful write and no audio. A settings app could not tell "the box is master,
# its preamps are preconfigured" from "your write worked".
#
# WHAT THIS MEASURES, on a real graph with two separate clients: that the four properties
# exist on the live node (a unit test cannot see a typo in the property seed), that a
# client which never wrote anything reads them, that a cold master answers `no-box` /
# `unavailable` rather than silence, and that the refused write CHANGED NOTHING — the
# asserted list is still empty afterwards, so the cell reached no send table.
#
# WHAT IT CANNOT MEASURE, honestly: the ACCEPTED path. That needs a box enrolled with us,
# and no emulator in this tree grants one (tests/fake_box_master.c is a box on M, which is
# the refusal case). The accepted write -> asserted readback -> wire bytes chain is proven
# offline against libreac's captured golden in tests/test_reac_headamp_state.c, and on the
# rig by a measured gain change on a box's audio.
#
# THE WRITE PATH IS REACHED, and that is sabotage-verified rather than assumed: with the
# capability gate forced open, this same test goes red with `a REFUSED write entered the
# asserted table anyway: '32:0=1'`. So the cell does arrive at the node over the graph, and
# what keeps it off the wire is the refusal — not a write that never landed.
#
# PRESENCE BEFORE ABSENCE. An empty property value and a missing key read identically off
# pw-dump, and this test claims things about both, so the probe first proves it can read a
# property whose value it already knows, and every read below reports `(missing)` for an
# absent key rather than an empty string.
#
# Isolation is part of the test: an unprivileged user+net+pid namespace AND a private
# PipeWire, so nothing here reaches the operator's live graph or a real REAC segment. The
# wire's far end lives in a nested namespace, so our end has carrier and carries not one
# frame. Skips (77) where the namespaces, iproute2 or PipeWire are unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter  >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v python3  >/dev/null 2>&1 || { echo "SKIP: no python3"; exit $SKIP; }
command -v ip       >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump  >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
command -v pw-cli   >/dev/null 2>&1 || { echo "SKIP: no pw-cli"; exit $SKIP; }
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

# THE READER — a SECOND client, which never writes. It resolves the daemon's own nodes
# through client.id so nothing else in this namespace can be mistaken for its work, and it
# prints `(missing)` for a key that is absent, because an absent key and an empty value are
# the two things this test has to tell apart.
read_prop() {   # read_prop <daemon-pid> <node.name> <property>
	pw-dump | python3 -c '
import json,sys
pid,name,key=int(sys.argv[1]),sys.argv[2],sys.argv[3]
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    if p.get("node.name")!=name: continue
    print(p[key] if key in p else "(missing)")
    break
' "$1" "$2" "$3"
}
node_id() {     # node_id <daemon-pid> <node.name>
	pw-dump | python3 -c '
import json,sys
pid,name=int(sys.argv[1]),sys.argv[2]
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) in mine and p.get("node.name")==name:
        print(o["id"]); break
' "$1" "$2"
}

# ---- ONE WIRE WITH NOTHING ON IT. The peer end goes to a nested namespace while it is
# still DOWN and is raised there, so our end gets carrier and the wire stays silent.
unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
ip link add ha0 type veth peer name pha0 || exit 90
ip link set pha0 netns $NSPID || exit 90

mkdir -p "$CONF/.config/reac-pw"
printf '[segment ha0]\nrole = master\n' > "$CONF/.config/reac-pw/reac-pw.conf"

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "FAIL: the daemon never got running"; tail -5 "$LOG"; exit 1; }

ip link set ha0 up; nsenter -t $NSPID -n ip link set pha0 up

NODE=reac-playback.ha0
for ((i = 0; i < 60; i++)); do
	[ -n "$(node_id $PID $NODE)" ] && break
	sleep 0.5
done
ID=$(node_id $PID $NODE)
[ -n "$ID" ] || {
	echo "FAIL: a pinned master with no box published no $NODE — there is no door to"
	echo "      read a head-amp answer off at all"; tail -25 "$LOG"; exit 1; }

# ---- 0. THE PROBE'S POSITIVE CONTROL. Before claiming anything about a property's value,
# prove this reader can read a property whose value is already known. A probe that cannot
# see a key it is looking straight at reports every absence identically.
SEG=$(read_prop $PID $NODE reac.segment)
[ "$SEG" = "ha0" ] || {
	echo "FAIL: the reader cannot read a property it already knows the value of"
	echo "      (reac.segment = '$SEG', expected 'ha0') — it cannot testify about any"
	exit 1; }
CAPS=$(read_prop $PID $NODE reac.headamp.caps)
[ "$CAPS" = "phantom,pad,sens" ] || {
	echo "FAIL: reac.headamp.caps reads '$CAPS' — the capability trio a client builds its"
	echo "      rows from is not on the node"; exit 1; }
echo "OK: the reader sees $NODE and reads its properties (caps '$CAPS')"

# ---- 1. THE TRAVEL IS PUBLISHED, so a client renders the range it receives rather than
# one compiled into it (ruling 1). 55 = 0x37 = REAC_HEADAMP_SENS_MAX.
SENSMAX=$(read_prop $PID $NODE reac.headamp.sens.max)
[ "$SENSMAX" = "55" ] || {
	echo "FAIL: reac.headamp.sens.max reads '$SENSMAX', not the published travel '55'"
	exit 1; }

# ---- 2. A COLD MASTER ANSWERS, AND THE ANSWER IS HONEST. No box is recognised, so there
# are no preamps to address and the node says so in both keys rather than going quiet.
STATE=$(read_prop $PID $NODE reac.headamp.state)
REFUSED=$(read_prop $PID $NODE reac.headamp.refused)
ASSERTED=$(read_prop $PID $NODE reac.headamp.asserted)
[ "$STATE" = "unavailable" ] || {
	echo "FAIL: a master with no box answers reac.headamp.state '$STATE'"; exit 1; }
[ "$REFUSED" = "no-box" ] || {
	echo "FAIL: a master with no box answers reac.headamp.refused '$REFUSED'"; exit 1; }
# PRESENT AND EMPTY, not absent: a client must be able to tell "this daemon asserts
# nothing" from "this daemon does not publish a readback at all".
[ "$ASSERTED" = "" ] || {
	echo "FAIL: nothing has been written and reac.headamp.asserted reads '$ASSERTED'"
	echo "      ('(missing)' means the key is not published at all)"; exit 1; }
echo "OK: a cold master answers state=unavailable refused=no-box asserted='' (present, empty)"

# ---- 3. ONE CLIENT WRITES, THE OTHER READS THE REFUSAL. pw-cli is a separate process with
# its own connection to the graph; it exits 0 whatever the node does with the cell, which is
# precisely why the answer has to be a property and not a return code.
pw-cli set-param "$ID" Props '{ params = [ "reac.headamp.32.phantom", 1 ] }' >/dev/null 2>&1
sleep 1
STATE2=$(read_prop $PID $NODE reac.headamp.state)
REFUSED2=$(read_prop $PID $NODE reac.headamp.refused)
ASSERTED2=$(read_prop $PID $NODE reac.headamp.asserted)
[ "$REFUSED2" = "no-box" ] || {
	echo "FAIL: after a write to a segment with no preamps the refusal reads '$REFUSED2'"
	exit 1; }
[ "$STATE2" = "unavailable" ] || {
	echo "FAIL: after a refused write the state reads '$STATE2'"; exit 1; }
# AND THE REFUSAL MOVED NOTHING. This is the half that a returned code could never show:
# the cell did not enter the table this daemon re-pushes at establishment, so there is no
# setting waiting to be sent to a box that arrives later.
[ "$ASSERTED2" = "" ] || {
	echo "FAIL: a REFUSED write entered the asserted table anyway: '$ASSERTED2'"
	echo "      — the daemon would push a cell it told the caller it had refused"; exit 1; }
echo "OK: a write refused as no-box is read back by a second client, and asserted nothing"

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
echo "PASS"
INNER
)
rc=$?
echo "$OUT"
case "$OUT" in
	*SKIP:*) exit $SKIP ;;
esac
[ $rc -eq 0 ] || exit $rc
case "$OUT" in
	*PASS*) exit 0 ;;
	*) echo "FAIL: the inner namespace did not reach PASS"; exit 1 ;;
esac
