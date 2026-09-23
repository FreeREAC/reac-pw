#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: an enrolled box's reac-capture / reac-playback pair OUTLIVES A PIPEWIRE
# RESTART — the daemon notices its nodes died with the server and builds them again.
#
# THE FAULT, ON THE DESK 2026-09-23 (docs/design/evidence/reac-pw-boot-2026-09-23.log).
# The S-1608 on enp131s0 enrolled at 13:44:01 and the journal read "autodetected S-1608
# (16 in / 8 out) -> reac-capture 16 in / reac-playback 8 out". At 13:44:03 systemd stopped
# pipewire.service and started it again (openmixer's engine exits on a lost connection and
# the user session restarts the stack). Twelve minutes later the box's REAC LED still said
# enrolled, the master was still pacing its wire, the roster row still read `established
# 16/8` — and the graph held NO node for that segment, so the console had no device and
# the desk was 48 links short. Not one line said so: the node-recover ladder judged "on the
# graph" from a pw_stream whose server had gone away, and a stream that has lost its server
# is neither in ERROR nor without a node id. It is UNCONNECTED, and nothing read that.
#
# PRESENCE BEFORE ABSENCE. Phase 1 — the box enrolled and the pair on the graph — is the
# control; a rebuild reported by an instrument that never saw the pair means nothing.
#
#   1. A BOX ARRIVES (libreac's fake_box on the far end). reac-capture AND reac-playback
#      carry the segment.
#   2. THE SERVER GOES AND COMES BACK: the private PipeWire is killed and a fresh one is
#      started on the same runtime dir. Nothing else changes — the box keeps talking, the
#      daemon keeps mastering.
#   3. THE PAIR IS BACK, with NEW node ids, inside the recover ladder's window, and the
#      daemon SAID it rebuilt them.
#
# ISOLATION as in no-box-no-node.sh: a user+net+mount+pid namespace with its own veth,
# its own sysfs and its own PipeWire on a private runtime dir.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
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

start_pipewire() {
	pipewire >>"$RT/pw.log" 2>&1 &
	PWPID=$!
	for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && return 0; sleep 0.2; done
	return 1
}
start_pipewire || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# ONE LINE PER NODE THIS DAEMON OWNS: <node.id> <node.name> <reac.segment>. Resolved
# node -> client.id -> client -> application.process.id, so nothing else in this namespace
# can be mistaken for its work. The id is printed because a REBUILT node is a NEW id: the
# same ids after the restart would be a stale dump, not a recovery.
nodes_of() {
	pw-dump 2>/dev/null | python3 -c '
import json,sys
pid=int(sys.argv[1])
try: d=json.load(sys.stdin)
except Exception: sys.exit(0)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    i=o["info"]; p=i["props"]
    if int(p.get("client.id",-1)) not in mine: continue
    print(o["id"], p.get("node.name","?"), p.get("reac.segment","(none)"))
' "$1"
}
seg_nodes() { nodes_of "$1" | awk -v s="$2" '$3 == s'; }

ip link add gpr0 type veth peer name gprb0 || exit 90
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set gprb0 netns $NSPID || exit 90
ip link set gpr0 up; $in_peer ip link set gprb0 up

HOME="$CONF" "$BIN" --live gpr0 --tx gpr0 --name gpr0 --rate 96000 >"$LOG" 2>&1 &
PID=$!
sleep 4
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }

# ---- 1. A BOX ARRIVES: THE CONTROL ------------------------------------------------------
$in_peer "$FAKE" gprb0 180 >"$RT/box.log" 2>&1 &
FAKEPID=$!
for ((i = 0; i < 120; i++)); do
	[ "$(seg_nodes $PID gpr0 | wc -l)" -ge 2 ] && break
	sleep 0.5
done
sleep 2
echo "box-count $(seg_nodes $PID gpr0 | wc -l)"
seg_nodes $PID gpr0 | sed 's/^/  box-node /'
grep -a "autodetected" "$LOG" | sed 's/^/  box-log /' | head -2
BEFORE_IDS=$(seg_nodes $PID gpr0 | awk '{print $1}' | sort | tr '\n' ' ')
echo "before-ids $BEFORE_IDS"
# THE ROSTER NODE IS THE SAME KIND OF CASUALTY: it lives on its own core, and on the desk
# it stayed a dead handle for seven minutes after the restart, until a segment happened to
# leave. Counted by node.name on this daemon's own client, before and after.
echo "before-roster $(nodes_of $PID | awk '$2 == "reac-pw"' | wc -l)"

# ---- 2. THE SERVER GOES AND COMES BACK ----------------------------------------------------
kill -TERM $PWPID 2>/dev/null; sleep 0.5; kill -9 $PWPID 2>/dev/null; wait $PWPID 2>/dev/null
echo "server-killed 1"
sleep 1
start_pipewire || { echo "server-restart-failed 1"; tail -3 "$RT/pw.log"; exit 92; }
echo "server-back 1"
kill -0 $PID 2>/dev/null || { echo "daemon-died-on-restart 1"; tail -8 "$LOG"; exit 93; }

# ---- 3. THE PAIR IS BACK ------------------------------------------------------------------
# The recover ladder polls every 200 ms with a 10-tick grace and rebuilds; 30 s is ten
# times what a working daemon needs and far under what an operator would call recovered.
for ((i = 0; i < 60; i++)); do
	[ "$(seg_nodes $PID gpr0 | wc -l)" -ge 2 ] && break
	sleep 0.5
done
sleep 1
echo "after-count $(seg_nodes $PID gpr0 | wc -l)"
seg_nodes $PID gpr0 | sed 's/^/  after-node /'
AFTER_IDS=$(seg_nodes $PID gpr0 | awk '{print $1}' | sort | tr '\n' ' ')
echo "after-ids $AFTER_IDS"
echo "daemon-alive $(kill -0 $PID 2>/dev/null && echo 1 || echo 0)"
for ((i = 0; i < 40; i++)); do
	[ "$(nodes_of $PID | awk '$2 == "reac-pw"' | wc -l)" -ge 1 ] && break
	sleep 0.5
done
echo "after-roster $(nodes_of $PID | awk '$2 == "reac-pw"' | wc -l)"
grep -a "rebuilding it\|back on the graph\|went away\|stream ERROR" "$LOG" | sed 's/^/  restart-log /' | head -8
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
val() { echo "$OUT" | grep -a "^$1 " | head -1 | cut -d' ' -f2-; }

echo "$OUT" | grep -qa '^daemon-died' && fail "the daemon died at start"

# 0. THE CONTROL FIRST.
[ "$(val box-count)" = "2" ] \
	|| fail "a box enrolled and did NOT bring reac-capture + reac-playback (got $(val box-count)) — no presence shown, so no absence below is a measurement"
echo "$OUT" | grep -qa 'box-log.*autodetected' \
	|| fail "the box never enrolled, so this run has shown no presence"
[ "$(val server-back)" = "1" ] || fail "the private PipeWire could not be restarted — the fixture, not the daemon"

# 1. THE DAEMON SURVIVES ITS SERVER.
[ "$(val daemon-alive)" = "1" ] || fail "the daemon died when PipeWire restarted"

# 2. THE PAIR IS BACK, AND IT IS A NEW PAIR.
[ "$(val after-count)" = "2" ] \
	|| fail "PipeWire restarted and the enrolled box's pair did not come back (got $(val after-count) node(s) after 30 s) — this is the desk on 2026-09-23: a box whose LED says enrolled and a graph with nothing for it"
[ "$(val before-ids)" != "$(val after-ids)" ] \
	|| fail "the node ids after the restart are the ids before it ($(val after-ids)) — a stale dump, not a rebuilt pair"

# 3. THE ROSTER NODE IS BACK TOO — the console derives its segments from it.
[ "$(val before-roster)" = "1" ] || fail "no roster node before the restart (got $(val before-roster)) — the fixture never showed one, so its return cannot be measured"
[ "$(val after-roster)" = "1" ] \
	|| fail "PipeWire restarted and the roster node did not come back (got $(val after-roster) after 20 s) — on the desk it stayed a dead handle until a segment happened to leave"

# 4. AND THE DAEMON SAID SO — a rebuild nobody announces is the next silent failure.
echo "$OUT" | grep -qa 'restart-log.*rebuilding it' \
	|| fail "the pair was rebuilt and the journal never said 'rebuilding it'"
echo "$OUT" | grep -qa 'restart-log.*back on the graph' \
	|| fail "the pair was rebuilt and the journal never said it is back on the graph"

echo "OK"
exit 0
