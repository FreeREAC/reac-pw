#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A BOX WITH NO OUTPUTS PUTS NO AUDIO SINK ON THE GRAPH, AND ITS SEGMENT'S
# DOOR IS reac-capture.
#
# THE FAULT, ON THE RIG 2026-10-08. An S-4000S-4000 declared 40 inputs and 0 outputs on
# msi's USB segment. reac-capture came up at 40 ports, and beside it stood
# reac-playback.<segment>: media.class Audio/Sink, two input ports and two monitors,
# described as "REAC segment door (no box recognized yet)" while carrying
# reac.box-width=40x0. A stream asked for at zero channels is not a node without ports;
# the graph's adapter gives it a stereo pair, and the console listed it as a stereo
# output the box does not have. The design (38b4e7f, 2026-10-06) is that such a box has
# no reac-playback at all and its reac-capture carries the segment. So this checks both
# halves: no sink, and the door's props and its write side on reac-capture.
#
#   ARM 1  fake_box fr4000 (40 in / 0 out). Every node this daemon owns is listed with
#          the graph's own port counts. reac-capture has 40 ports; no node of this daemon
#          is an Audio/Sink or is named reac-playback; reac-capture carries the segment's
#          answers (reac.master.state, reac.rate, reac.cfg.role.state, reac.headamp.*,
#          reac.discovery.scope); and a reac.cfg.rate write sent to reac-capture is
#          answered there (a malformed value reads back reac.cfg.rate.refused=malformed).
#   ARM 3  the same row pinned (--box fr4000) on a silent wire: the pin builds the nodes
#          at boot, and must not build a sink either.
#   ARM 4  a 16 / 8 box whose first reac-playback is refused (tests/refuse_playback_shim.c):
#          reac-capture carries the door until reac-playback is rebuilt, then carries
#          none of it.
#   ARM 2  the control, fake_box s1608 (16 in / 8 out), on the same harness: a
#          reac-playback with 8 input ports does exist and carries those answers. A probe
#          that never finds a sink proves nothing about a sink's absence.
#
# ISOLATION: as tests/box-0832-enrols.sh — an unprivileged user+net+mount+pid namespace
# with its own PipeWire on a private runtime dir, the far end of the veth in a nested
# network namespace. Nothing here touches the live graph.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box /path/to/refuse-playback-shim.so}"
FAKE="${2:-}"
SHIM="${3:-}"
# ABSOLUTE: the far end runs under nsenter in its own mount namespace, which starts at /.
BIN=$(readlink -f "$BIN"); [ -n "$FAKE" ] && FAKE=$(readlink -f "$FAKE")
[ -n "$SHIM" ] && SHIM=$(readlink -f "$SHIM")
SKIP=77

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
[ -n "$SHIM" ] && [ -f "$SHIM" ] || { echo "SKIP: no refuse-playback shim at '$SHIM'"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

SECS="${REACPW_DOOR_SECS:-30}"

run_arm() {   # run_arm <model-token>
	unshare -r -n -m -p -f --mount-proc --map-root-user \
		bash -s -- "$BIN" "$FAKE" "$SECS" "$1" "$SHIM" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; SECS="$3"; MODEL="$4"; SHIM="$5"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"

ip link add dor0 type veth peer name dorb0 || exit 90
ip link set dorb0 netns $NSPID || exit 90
ip link set dor0 up; $in_peer ip link set dorb0 up

# `pin:<token>` is a silent wire with that box PINNED (--box): no far end at all, the
# nodes built at boot from the pin. `shim:<token>` is that far end with the daemon under
# the refuse-playback shim (its first reac-playback is refused). Any other word is the far
# end's model token.
PIN=(); PRELOAD=
case "$MODEL" in
pin:*)  PIN=(--box "${MODEL#pin:}"); : >"$RT/box.log"; sleep 900 & FAKEPID=$! ;;
shim:*) PRELOAD="$SHIM"
        $in_peer "$FAKE" dorb0 "$SECS" "${MODEL#shim:}" >"$RT/box.log" 2>&1 & FAKEPID=$! ;;
*)      $in_peer "$FAKE" dorb0 "$SECS" "$MODEL" >"$RT/box.log" 2>&1 & FAKEPID=$! ;;
esac

HOME="$CONF" LD_PRELOAD="$PRELOAD" "$BIN" --live dor0 --tx dor0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" \
	--name dor0 "${PIN[@]}" >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || { echo "daemon exited early"; tail -5 "$LOG"; exit 91; }

for ((i = 0; i < SECS * 2; i++)); do
	case "$MODEL" in pin:*) break;; esac
	grep -q "ESTABLISHED" "$LOG" && break
	sleep 0.5
done
# Under the shim, the recovery ladder has to rebuild the refused reac-playback first.
case "$MODEL" in shim:*)
	for ((i = 0; i < SECS * 2; i++)); do
		grep -q "is back on the graph" "$LOG" && break
		sleep 0.5
	done;;
esac
sleep 3

# ONE LINE PER NODE THIS DAEMON OWNS: NODE <id> <node.name> <media.class> <in> <out>,
# then PROP <node.name> <key>=<value> for every reac.* key. Resolved node -> client.id ->
# client -> application.process.id, so nothing else in this namespace can be mistaken
# for its work. The port counts are the graph's own, not a prop the daemon writes.
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
    name=p.get("node.name","?")
    print("NODE", o["id"], name, p.get("media.class","(none)"),
          i.get("n_input_ports",0), i.get("n_output_ports",0))
    for k in sorted(p):
        if k.startswith("reac."): print("PROP", name, "%s=%s" % (k, p[k]))
' "$1"
}
nodes_of $PID >"$RT/before.txt"

# THE WRITE SIDE OF THE DOOR. A malformed reac.cfg.rate is refused and moves nothing,
# so it is safe to send, and its answer can only appear on the node that took it.
# Under the shim the probe goes to reac-capture, which must NOT answer it any more.
DOOR=$(awk '$1=="NODE" && $3 ~ /^reac-capture/ {print $2}' "$RT/before.txt" | head -1)
[ "$MODEL" = s1608 ] && DOOR=$(awk '$1=="NODE" && $3 ~ /^reac-playback/ {print $2}' "$RT/before.txt" | head -1)
if [ -n "$DOOR" ]; then
	pw-cli set-param "$DOOR" Props '{ params = [ "reac.cfg.rate" "bogus" ] }' >/dev/null 2>&1
	# AND A HEAD-AMP WRITE on the 40 / 0 box's door: a sens on its first preamp, on a
	# fake box in this namespace. Read back as reac.headamp.asserted on that node.
	[ "$MODEL" = fr4000 ] &&
		pw-cli set-param "$DOOR" Props '{ params = [ "reac.headamp.0.sens" 20 ] }' >/dev/null 2>&1
	sleep 1.5
fi
nodes_of $PID >"$RT/after.txt"
kill -TERM $PID 2>/dev/null; sleep 0.5
case "$MODEL" in pin:*) kill $FAKEPID; BOXRC=0;; *) wait $FAKEPID; BOXRC=$?;; esac

echo "--- box ---"; cat "$RT/box.log"
echo "--- nodes ---"; grep '^NODE' "$RT/before.txt"
echo "--- door props ---"; grep '^PROP' "$RT/before.txt"
echo "--- after the write ---"; grep -E 'reac.cfg.rate.refused|reac.headamp.asserted' "$RT/after.txt"
echo "--- daemon ---"; grep -aE "autodetected|box declared|ESTABLISHED|COULD NOT|REFUSED|pinned|refuse-playback-shim|back on the graph|NOT on the graph" "$LOG" | tail -12
echo "BOXRC=$BOXRC"
INNER
}

FAIL=0
say() { printf '%s\n' "$*"; }
node_line() { echo "$1" | awk -v pre="$2" '$1=="NODE" && $3 ~ "^"pre'; }
has_prop() {  # has_prop <arm output> <node prefix> <key> [value]
	echo "$1" | awk -v pre="$2" -v k="$3" -v v="${4:-}" '
		$1=="PROP" && $2 ~ "^"pre { split($3, kv, "="); if (kv[1]==k && (v=="" || substr($3, length(k)+2)==v)) f=1 }
		END { exit f ? 0 : 1 }'
}
DOOR_KEYS="reac.segment reac.master.state reac.rate reac.cfg.role.state reac.headamp.caps reac.headamp.state reac.discovery.scope"

# ---- ARM 1: 40 in / 0 out.
A1=$(run_arm fr4000 2>&1) || true
case "$A1" in *"SKIP: "*) echo "${A1##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
say "$A1"
# The far end's own announcement (libreac's fake_box prints it), read with awk: the
# producer is not this daemon, so it is not one of the daemon's log lines.
echo "$A1" | awk -v w="$FACT_MAX_CHANNELS" 'index($0, "fake_box: declaring as") && index($0, " " w " in / 0 out") { f = 1 } END { exit !f }' || {
	say "FAIL: the far end never declared $FACT_MAX_CHANNELS in / 0 out — nothing here is about that box"; FAIL=1; }
echo "$A1" | grep -q "ESTABLISHED" || { say "FAIL: the master never reached ESTABLISHED"; FAIL=1; }
# THE PORT COUNTS ARE PRINTED AND NOT ASSERTED: a private PipeWire with no session
# manager reports 0 ports on these nodes (tests/no-box-no-node.sh measured it). What is
# asserted is which nodes exist and their media.class; the widths are read on the rig.
CAP=$(node_line "$A1" reac-capture)
echo "$CAP" | awk '{ exit ($4=="Audio/Source") ? 0 : 1 }' || {
	say "FAIL: no Audio/Source reac-capture for the 40 / 0 box: '$CAP'"; FAIL=1; }
node_line "$A1" reac-playback | grep -q . && {
	say "FAIL: a box with no outputs has a reac-playback: $(node_line "$A1" reac-playback)"; FAIL=1; }
echo "$A1" | awk '$1=="NODE" && $4=="Audio/Sink"' | grep -q . && {
	say "FAIL: a box with no outputs left an Audio/Sink on the graph"; FAIL=1; }
for k in $DOOR_KEYS; do
	has_prop "$A1" reac-capture "$k" || { say "FAIL: reac-capture, the door, does not carry $k"; FAIL=1; }
done
has_prop "$A1" reac-capture reac.master.state us || {
	say "FAIL: the door does not say this daemon masters the segment"; FAIL=1; }
has_prop "$A1" reac-capture reac.headamp.channels "$FACT_MAX_CHANNELS" || {
	say "FAIL: the door does not publish the box's $FACT_MAX_CHANNELS preamps"; FAIL=1; }
echo "$A1" | sed -n '/^--- after the write ---/,/^---/p' | grep -q "reac.cfg.rate.refused=malformed" || {
	say "FAIL: a reac.cfg.rate write sent to reac-capture was not answered there"; FAIL=1; }
echo "$A1" | sed -n '/^--- after the write ---/,/^---/p' | awk '$3 ~ /^reac.headamp.asserted=./ && $2 ~ /^reac-capture/ { f = 1 } END { exit !f }' || {
	say "FAIL: a reac.headamp write sent to reac-capture was not asserted there"; FAIL=1; }
echo "$A1" | grep -q "BOXRC=0" || { say "FAIL: the 40 / 0 box never enrolled (fake_box exit != 0)"; FAIL=1; }

# ---- ARM 2: the control, a box with outputs.
A2=$(run_arm s1608 2>&1) || true
case "$A2" in *"SKIP: "*) say "SKIP: the control arm could not run"; exit $SKIP;; esac
say "$A2"
PB=$(node_line "$A2" reac-playback)
echo "$PB" | awk '{ exit ($4=="Audio/Sink") ? 0 : 1 }' || {
	say "FAIL (control): no Audio/Sink reac-playback — this probe cannot see a sink: '$PB'"; FAIL=1; }
for k in $DOOR_KEYS; do
	has_prop "$A2" reac-playback "$k" || { say "FAIL (control): reac-playback does not carry $k"; FAIL=1; }
done
echo "$A2" | sed -n '/^--- after the write ---/,/^---/p' | grep -q "reac.cfg.rate.refused=malformed" || {
	say "FAIL (control): the write probe got no answer from reac-playback either"; FAIL=1; }

# ---- ARM 3: the same box PINNED on a silent wire (--box fr4000). The pin builds the
# nodes at boot through reac_sink_node_ensure at the row's 0 outputs, which is the call
# that made the stereo sink.
A3=$(run_arm pin:fr4000 2>&1) || true
case "$A3" in *"SKIP: "*) say "SKIP: the pinned arm could not run"; exit $SKIP;; esac
say "$A3"
node_line "$A3" reac-capture | grep -q . || {
	say "FAIL (pin): the pinned 40 / 0 box has no reac-capture, so its absence of a sink proves nothing"; FAIL=1; }
node_line "$A3" reac-playback | grep -q . && {
	say "FAIL (pin): a pinned box with no outputs has a reac-playback: $(node_line "$A3" reac-playback)"; FAIL=1; }
echo "$A3" | awk '$1=="NODE" && $4=="Audio/Sink"' | grep -q . && {
	say "FAIL (pin): a pinned box with no outputs left an Audio/Sink on the graph"; FAIL=1; }

# ---- ARM 4: a box WITH outputs whose first reac-playback is refused (the shim). For that
# window reac-capture is the door; once the recovery ladder rebuilds reac-playback, the
# door must be reac-playback alone: no master state, role, rate or head-amp answer left on
# reac-capture, and a write to reac-capture taken by nobody.
A4=$(run_arm shim:s1608 2>&1) || true
case "$A4" in *"SKIP: "*) say "SKIP: the shim arm could not run"; exit $SKIP;; esac
say "$A4"
# The shim's own line (tests/refuse_playback_shim.c prints it, not the daemon), read with awk.
echo "$A4" | awk 'index($0, "refuse-playback-shim: refused") { f = 1 } END { exit !f }' || {
	say "FAIL (shim): the first reac-playback was never refused, so this arm tested nothing"; FAIL=1; }
echo "$A4" | grep -q "is back on the graph" || {
	say "FAIL (shim): reac-playback was never rebuilt after the refusal"; FAIL=1; }
node_line "$A4" reac-playback | awk '{ exit ($4=="Audio/Sink") ? 0 : 1 }' || {
	say "FAIL (shim): no reac-playback after the rebuild"; FAIL=1; }
has_prop "$A4" reac-playback reac.master.state us || {
	say "FAIL (shim): the rebuilt reac-playback is not the door"; FAIL=1; }
# EMPTIED, not removed: a stream's property update merges on the server, so a key can be
# emptied and not taken away. An empty value is no answer, which is what is checked.
for k in reac.master.state reac.cfg.role.state reac.rate reac.headamp.caps reac.headamp.state reac.discovery.scope; do
	echo "$A4" | awk -v k="$k" '$1=="PROP" && $2 ~ /^reac-capture/ && index($3, k "=") == 1 && length($3) > length(k) + 1 { f = 1 } END { exit !f }' &&
		{ say "FAIL (shim): reac-capture still carries $k beside the rebuilt reac-playback — a second door"; FAIL=1; }
done
echo "$A4" | sed -n '/^--- after the write ---/,/^---/p' | grep -qE "reac-(capture|playback).* reac.cfg.rate.refused=malformed" && {
	say "FAIL (shim): a reac.cfg.rate write to reac-capture was still taken as the door's"; FAIL=1; }

[ "$FAIL" = 0 ] && say "PASS: a 40 / 0 box, wired or pinned, has reac-capture and no sink, and its door on reac-capture; a 16 / 8 box keeps its reac-playback door, also after a refused first build"
exit $FAIL
