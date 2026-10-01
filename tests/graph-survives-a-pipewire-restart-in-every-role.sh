#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a SLAVE, a TAP and a PINNED segment's nodes outlive a PipeWire restart
# too (audit 2026-09-24, H2 and M2).
#
# graph-survives-a-pipewire-restart.sh holds the master autodetect pair to it. The ladder
# that rebuilds them ran from the autodetect timer, which only a master segment has, so
# every other role's nodes died with the server and stayed dead, with no line. This is
# the same fixture with the other two engines behind it:
#
#   slave  libreac-shaped box master on the far end (tests/fake_box_master.c); the daemon
#          autodetects the wire and JOINS it as a slave -- reac-capture AND reac-playback.
#   tap    the same wire, with `role = tap` in reac-pw.conf -- one reac-capture per heard
#          stream, nothing transmitted.
#   pinned a master with `--box s1608` and NO box on the wire: the pin keeps the pair on
#          the graph before the box is powered, and its ladder ran only once a box was
#          recognized (M2), so a restart while the box was off killed the patch.
#
# PRESENCE BEFORE ABSENCE: the nodes must be on the graph before the restart, or their
# absence after it measures nothing. The proof is NEW node ids for every node the segment
# had, and the daemon's own "rebuilding it" / "back on the graph" lines.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>, exported into the namespace body
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master slave|tap|pinned}"
FAKE="${2:-}"
MODE="${3:?usage: $0 /path/to/reac-pw /path/to/fake-box-master slave|tap|pinned}"
SKIP=77

case "$MODE" in slave|tap|pinned) ;; *) echo "FAIL: mode '$MODE' is not slave, tap or pinned"; exit 1 ;; esac
[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake-box-master at '$FAKE'"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" "$MODE" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; MODE="$3"
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

# <id> <node.name> <reac.segment> for every node THIS daemon owns.
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

ip link add grs0 type veth peer name grsb0 || exit 90
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set grsb0 netns $NSPID || exit 90

# The box masters the wire before the carrier exists, so the daemon hears a master from
# the first instant and never races to master it itself.
[ "$MODE" = pinned ] ||
	$in_peer "$FAKE" grsb0 00:40:ab:c4:08:bc "$FACT_BOX_S0808_IN" 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
sleep 0.5
ip link set grs0 up; $in_peer ip link set grsb0 up

mkdir -p "$CONF/.config/reac-pw"
[ "$MODE" = tap ] && printf '[segment grs0]\nrole = tap\n' > "$CONF/.config/reac-pw/reac-pw.conf"
WANT=2; [ "$MODE" = tap ] && WANT=1
ARGS=(); [ "$MODE" = pinned ] && ARGS=(--live grs0 --tx grs0 --name grs0 --box s1608)
HOME="$CONF" REACPW_BOX_MASTER_FRAME=mixer "$BIN" "${ARGS[@]}" >"$LOG" 2>&1 &
PID=$!
for ((i = 0; i < 80; i++)); do
	[ "$(seg_nodes $PID grs0 | wc -l)" -ge $WANT ] && break
	kill -0 $PID 2>/dev/null || break
	sleep 0.5
done
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }
sleep 2
echo "want $WANT"
echo "before-count $(seg_nodes $PID grs0 | wc -l)"
seg_nodes $PID grs0 | sed 's/^/  before-node /'
echo "before-ids $(seg_nodes $PID grs0 | awk '{print $1}' | sort | tr '\n' ' ')"

kill -TERM $PWPID 2>/dev/null; sleep 0.5; kill -9 $PWPID 2>/dev/null; wait $PWPID 2>/dev/null
sleep 1
start_pipewire || { echo "server-restart-failed 1"; tail -3 "$RT/pw.log"; exit 92; }
echo "server-back 1"

for ((i = 0; i < 80; i++)); do
	[ "$(seg_nodes $PID grs0 | wc -l)" -ge $WANT ] && break
	sleep 0.5
done
sleep 1
echo "daemon-alive $(kill -0 $PID 2>/dev/null && echo 1 || echo 0)"
echo "after-count $(seg_nodes $PID grs0 | wc -l)"
seg_nodes $PID grs0 | sed 's/^/  after-node /'
echo "after-ids $(seg_nodes $PID grs0 | awk '{print $1}' | sort | tr '\n' ' ')"
grep -a "rebuilding it\|back on the graph\|stream ERROR" "$LOG" | sed 's/^/  restart-log /' | head -8
exit 0
INNER
)
rc=$?
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL ($MODE): $1"; exit 1; }
val() { echo "$OUT" | grep -a "^$1 " | head -1 | cut -d' ' -f2-; }

W=$(val want)
[ "$(val before-count)" = "$W" ] \
	|| fail "the $MODE segment never put its $W node(s) on the graph (got $(val before-count)) -- no presence shown, so no absence below is a measurement"
[ "$(val server-back)" = "1" ] || fail "the private PipeWire could not be restarted -- the fixture, not the daemon"
[ "$(val daemon-alive)" = "1" ] || fail "the daemon died when PipeWire restarted"
[ "$(val after-count)" = "$W" ] \
	|| fail "PipeWire restarted and the $MODE segment's node(s) did not come back (got $(val after-count) of $W after 40 s)"
[ "$(val before-ids)" != "$(val after-ids)" ] \
	|| fail "the node ids after the restart are the ids before it ($(val after-ids)) -- a stale dump, not a rebuild"
echo "$OUT" | grep -qa 'restart-log.*rebuilding it' \
	|| fail "the node(s) came back and the journal never said 'rebuilding it'"
echo "$OUT" | grep -qa 'restart-log.*back on the graph' \
	|| fail "the node(s) came back and the journal never said they are back on the graph"
echo "OK"
exit 0
