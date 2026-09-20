#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: when a segment LEAVES, its indexed keys leave the roster node with it —
# `reac.roster.n` and the `reac.roster.<i>.*` groups on the graph agree, always.
# (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
# 2026-09-20 §b; reac-pw#106.)
#
# THE FAULT, ON THE DESK 2026-09-20. After the trunk lost carrier and every segment
# dropped, the roster node read `reac.roster.n = 0` beside FOUR live groups — one of them
# `established 32/8` on a wire with no carrier, and two of them the same interface name at
# two different indexes. `n` was right and the keys were a previous generation's.
#
# WHY A UNIT TEST COULD NOT SEE IT, and this is the whole reason this file exists.
# tests/test_reac_roster.c proves the DIFF the daemon computes, removals included, and it
# was green the entire time. What was wrong was one door further out: a PipeWire node
# cannot be told to DROP a property. The documented NULL-valued dict item is applied to the
# CLIENT's copy, the filter then publishes the SURVIVING keys, and the server MERGES those
# into the node — a key that is merely absent from a merge is never removed. So the SETS
# landed (n fell to 0) and the REMOVALS evaporated. This test reads the node's properties
# off pw-dump — the graph, never the daemon's own diff — which is the only place that
# difference is visible.
#
# PRESENCE BEFORE ABSENCE. Phase 1 asserts the roster is POPULATED and that this reader can
# see a named group for the segment that is about to go. A reader that can see nothing
# reports the same clean answer as a daemon that removed everything, and an absence measured
# by an instrument that has never shown a presence is not a measurement.
#
# ISOLATION: a user+net+mount+pid namespace with its own veth, its own sysfs (sysfs does not
# follow a network namespace) and its own PipeWire on a private runtime dir, because
# `unshare -n` isolates the wire and not the graph. HOME is redirected at the daemon so the
# only conf it can read is the one written here — never the operator's.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
BIN=$(readlink -f "$BIN")
SKIP=77

for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# THE ROSTER, OFF THE GRAPH AND NOWHERE ELSE: node -> client.id -> client ->
# application.process.id, so nothing else in this namespace can be mistaken for its work.
# Prints three numbers and then every group's name, which is the whole question this test
# asks: `n <count> <named-groups> <indexes> <names...>`.
#
#   n            what reac.roster.n says
#   named        how many reac.roster.<i>.name keys the node actually carries
#   indexes      the highest index+1 any reac.roster.<i>.* key uses
#
# str() on the value is not a detail: pw-dump renders a numeric-looking property as a JSON
# number, so `== "1"` on a property the daemon set to the string "1" is false and the whole
# node reads as absent (measured 2026-09-16 in the sibling roster test).
roster_shape() {
	pw-dump | python3 -c '
import json,re,sys
pid=int(sys.argv[1])
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    if str(p.get("reac.roster")) != "1": continue
    n=str(p.get("reac.roster.n","?"))
    names={}
    top=0
    for k,v in p.items():
        m=re.match(r"^reac\.roster\.(\d+)\.(\w+)$",k)
        if not m: continue
        top=max(top,int(m.group(1))+1)
        if m.group(2)=="name": names[int(m.group(1))]=str(v)
    print("roster-shape", n, len(names), top,
          " ".join("%d:%s"%(i,names[i]) for i in sorted(names)))
' "$1"
}

# ---- ONE PARENT, TWO VLANS. Three segments the daemon can see; one of them is going to
#      be taken away underneath it, which is what a carrier loss does to a trunk's VLANs.
ip link add rk0 type veth peer name prk0 || exit 90
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 600' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set prk0 netns $NSPID || exit 90
ip link set rk0 up; $in_peer ip link set prk0 up
for v in 11 13; do
	ip link add link rk0 name rk0.$v type vlan id $v || exit 90
	$in_peer ip link add link prk0 name prk0.$v type vlan id $v || exit 90
	$in_peer ip link set prk0.$v up || exit 90
	ip link set rk0.$v up || exit 90
done

HOME="$CONF" "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }

# ---- 1. THE ROSTER IS POPULATED, AND THIS READER CAN SEE THE GROUP THAT IS ABOUT TO GO.
#         Everything below is an absence; without this line none of it is a measurement.
for ((i = 0; i < 60; i++)); do
	roster_shape $PID | grep -qa "rk0\.13" && break
	sleep 0.5
done
echo "before $(roster_shape $PID | sed -n '1p' | cut -d' ' -f2-)"

# ---- 2. THE SEGMENT GOES. An interface that is deleted is the sharpest form of the
#         carrier loss that produced #106, and the daemon's own DROP path is the same one.
ip link del rk0.13 || exit 90
for ((i = 0; i < 80; i++)); do
	roster_shape $PID | grep -qa "rk0\.13" || break
	sleep 0.5
done
sleep 2
echo "after $(roster_shape $PID | sed -n '1p' | cut -d' ' -f2-)"
echo "rosters $(roster_shape $PID | wc -l)"
grep -ca "S_SEGMENT_DROPPED.*\[rk0\.13\]\|\[rk0\.13\] segment dropped" "$LOG" \
	| sed 's/^/dropped /'
grep -a "E_ROSTER_REMOVE" "$LOG" | head -2 | sed 's/^/  roster-refusal /'
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
echo "$OUT" | sed 's/^/  /'
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"; exit $SKIP; }
echo "$OUT" | grep -qa '^SKIP:' && { echo "$OUT" | grep -a '^SKIP:'; exit $SKIP; }
echo "$OUT" | grep -qa '^daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }

fail() { echo "FAIL: $1"; exit 1; }
# `before`/`after` are: <n> <named> <top> <i:name ...>
f() { echo "$OUT" | grep -a "^ *$1 " | head -1 | awk -v c="$2" '{print $(c+1)}'; }

# ---- THE CONTROL, FIRST -----------------------------------------------------------------
echo "$OUT" | grep -qa '^ *before .*rk0\.13' \
	|| fail "the roster never listed rk0.13 — this reader has shown no presence, so its later silence about that segment means nothing (before: $(echo "$OUT" | grep -a '^ *before '))"
[ "$(f before 1)" -ge 2 ] 2>/dev/null \
	|| fail "reac.roster.n read '$(f before 1)' with three segments on the wire — the roster is not being published at all"
[ "$(f before 1)" = "$(f before 2)" ] \
	|| fail "before the drop, reac.roster.n=$(f before 1) already disagreed with the $(f before 2) named groups on the node — the roster was broken before this test's own act"
[ "$(f before 2)" = "$(f before 3)" ] \
	|| fail "before the drop, $(f before 2) named groups sat under $(f before 3) indexes — a group with no name is already a stale generation"
[ "$(f rosters 1)" = "1" ] \
	|| fail "$(f rosters 1) nodes carry reac.roster=1 — the daemon must publish exactly one roster node"

# ---- THE CLAIM ---------------------------------------------------------------------------
echo "$OUT" | grep -qa '^ *after .*rk0\.13' \
	&& fail "rk0.13 is gone from the wire and its group is STILL on the roster node: $(echo "$OUT" | grep -a '^ *after ')"
[ "$(f after 1)" = "$(f after 2)" ] \
	|| fail "reac.roster.n=$(f after 1) but the node carries $(f after 2) named groups — this is #106 exactly: the count is a SET and lands, the groups are REMOVALS and do not"
[ "$(f after 2)" = "$(f after 3)" ] \
	|| fail "$(f after 2) named groups sit under $(f after 3) indexes — a departed group left some of its keys behind"
[ "$(f after 1)" -lt "$(f before 1)" ] 2>/dev/null \
	|| fail "the roster did not shrink at all ($(f before 1) -> $(f after 1)): the segment never left the daemon's own tables, so nothing here is about the graph"

echo "OK"
exit 0
