#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a segment joined to a BOX MASTER, dropped by a link loss past the hold and
# heard again, CARRIES AUDIO AGAIN — in BOTH directions since 0.5.5, because a rejoin that
# receives and no longer enrols is a live segment on the console and a dead one at the box.
# Twice in a row.
#
# THE DEFECT THIS EXISTS AGAINST, measured on the rig 2026-09-09 13:36 with an S-0808 on M
# (/home/pau/.claude/jobs/87a4861e/tmp/s0808-rejoin.log): after `segment dropped — link down
# past the hold` and the re-hear ("segment up ... 3 served so far"), the re-created
# reac-capture.enp128s20f0u2 published DIGITAL SILENCE on all eight channels while the NIC
# was receiving 8019 frames a second of real samples, and its properties stayed at their
# create-time seeds — link-state=probing, master.state=none, pace.source=free-run,
# cfg.role.state=role_hunting. The first join of a daemon's life was always fine and a
# daemon restart always fixed it, which is what makes it a rejoin defect and not a join one.
#
# PRESENCE BEFORE ABSENCE. Every measure below is taken on the FIRST join before it is
# asked about a rejoin, so the probe is proven able to see the thing whose absence it
# reports. A frame counter that never printed a line and a frame counter stuck at zero read
# the same through `tail -1`, so the counter is read only from the log written AFTER the
# drop, never from whatever the previous listener left behind.
#
# ISOLATION IS PART OF THE TEST (see tests/hearing-finds-a-segment.sh's header): an
# unprivileged user+net+pid namespace AND a private PipeWire, because `unshare -n` isolates
# the wire and not the audio graph. The peer end of the veth lives in a NESTED network
# namespace so the daemon can never hear its own transmissions as another host's.
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
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
BIN="$1"
FAKE="$2"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

wait_for() {
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -q "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

# ONE NODE'S PUBLISHED ANSWER, '|'-separated, attributed to THIS daemon's client so a peer
# in the same namespace can never be mistaken for its work:
#   master.state|rival.kind|master.mac|segment|description|link-state|cfg.role.state|pace.source
node_props() {
	pw-dump | python3 -c '
import json,sys
pid, want = int(sys.argv[1]), sys.argv[2]
d = json.load(sys.stdin)
mine = {o["id"] for o in d if o.get("type") == "PipeWire:Interface:Client"
        and int(o["info"]["props"].get("application.process.id", -1)) == pid}
for o in d:
    if o.get("type") != "PipeWire:Interface:Node": continue
    p = o["info"]["props"]
    if int(p.get("client.id", -1)) not in mine: continue
    if p.get("node.name") != want: continue
    print("|".join([p.get("reac.master.state", "(none)"),
                    p.get("reac.master.rival.kind", "(none)"),
                    p.get("reac.master.mac", "(none)"),
                    p.get("reac.segment", "(none)"),
                    p.get("node.description", "(none)"),
                    p.get("reac.link-state", "(none)"),
                    p.get("reac.cfg.role.state", "(none)"),
                    p.get("reac.pace.source", "(none)")]))
' "$1" "$2"
}
fld() { echo "$1" | cut -d'|' -f"$2"; }

# One field of the emulator's report (it is replaced atomically at every write).
rep() { [ -s "$RT/box.rep" ] || return 1
        awk -v k="$1" '$1 == k { print $2; f = 1 } END { exit !f }' "$RT/box.rep"; }

# THE FEEDER'S OWN ACCEPTED-FRAME COUNT, read ONLY from the log written after line $1.
# reac_rx prints this line from inside the loop body that just decoded a frame, so a feeder
# that accepts nothing prints NOTHING — and `tail -1` over the whole file would then hand
# back the PREVIOUS listener's healthy number and call the defect a pass.
rx_ok_since() {   # rx_ok_since <first-line> <segment>
	tail -n "+$1" "$LOG" | sed -n "s/^reac_rx: \[$2\] ok=\([0-9]*\) .*/\1/p" | tail -1
}

# THE PEER IS ANOTHER HOST. Both veth ends in one namespace would let the daemon hear the
# fake box's frames on the box's own NIC and serve that end as a segment of its own.
unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }
in_peer="nsenter -t $NSPID -n"

ip link add rej0 type veth peer name rbox0 || exit 90
ip link set rbox0 netns $NSPID || exit 90

BOXMAC=00:40:ab:c4:dc:9c
# The box is on the wire before the carrier is, so the wire carries a box master from the
# first instant of link and the masterless licence is never in the race.
$in_peer "$FAKE" rbox0 "$BOXMAC" 8 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
FAKEPID=$!
sleep 0.5
ip link set rej0 up; peer ip link set rbox0 up

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- THE FIRST JOIN, which is also every probe's positive control. -----------------
wait_for "\[rej0\] box masters this wire" 20 || {
	echo "FAIL: a box mastered the wire and the daemon never joined it"
	tail -20 "$LOG"; tail -3 "$RT/box.log"; exit 1; }
wait_for "\[rej0\] segment up" 20 || {
	echo "FAIL: joined in the journal, but the segment never came up"; tail -20 "$LOG"; exit 1; }

# The four published facts, and the audio behind them. `established` is what the segment
# says once its own RX is accepting the box's frames, so these are one claim measured two
# ways: the graph's answer, and the feeder's count under it.
check_joined() {   # check_joined <cycle-name> <log-line-floor>
	local what="$1" floor="$2" P ok1 ok2 i
	for ((i = 0; i < 60; i++)); do
		P=$(node_props $PID reac-capture.rej0)
		[ -n "$P" ] && [ "$(fld "$P" 6)" = "established" ] && break
		sleep 0.5
	done
	[ -n "$P" ] || { echo "FAIL ($what): no reac-capture.rej0 on the graph at all"
		tail -20 "$LOG"; return 1; }
	[ "$(fld "$P" 6)" = "established" ] || {
		echo "FAIL ($what): the box is streaming and the segment never read established: $P"
		tail -20 "$LOG"; return 1; }
	[ "$(fld "$P" 1)" = "foreign" ] || {
		echo "FAIL ($what): master.state is not foreign: $P"; return 1; }
	[ "$(fld "$P" 2)" = "box" ] || {
		echo "FAIL ($what): master.rival.kind is not box: $P"; return 1; }
	[ "$(fld "$P" 3)" = "$BOXMAC" ] || {
		echo "FAIL ($what): the segment names a master other than the box: $P"; return 1; }
	[ "$(fld "$P" 7)" = "applied" ] || {
		echo "FAIL ($what): a locked join must publish cfg.role.state=applied: $P"; return 1; }
	[ "$(fld "$P" 8)" = "foreign-master" ] || {
		echo "FAIL ($what): pace.source must be foreign-master: $P"; return 1; }
	[ "$(fld "$P" 5)" = "REAC 8ch capture (box mic inputs)" ] || {
		echo "FAIL ($what): the segment is not sized to the box's own 8 ch: $P"; return 1; }
	# AND THE FRAMES ARE STILL ARRIVING, counted only from the log this cycle wrote.
	ok1=$(rx_ok_since "$floor" rej0)
	sleep 2.5
	ok2=$(rx_ok_since "$floor" rej0)
	[ -n "$ok2" ] || {
		echo "FAIL ($what): the feeder printed no accepted-frame line at all after the drop,"
		echo "      so it decoded NOTHING of the ~8000 frames a second on the wire"
		tail -n "+$floor" "$LOG" | tail -20; return 1; }
	[ "$ok2" -gt "${ok1:-0}" ] || {
		echo "FAIL ($what): rx.frames_ok is not advancing ($ok1 -> $ok2) while the box"
		echo "      floods the wire — the ports it published carry digital silence"
		tail -n "+$floor" "$LOG" | tail -20; return 1; }
	# AND THE ENROLMENT CAME BACK WITH IT (0.5.6). A rejoin that receives but no longer
	# sends is the same segment on a console and a dead one at the box: its outputs stop.
	# Measured at the far end, over the same window as the reception above — the UPSTREAM
	# we unicast to the box master, which is what reaches its outputs.
	local d1 d2
	d1=$(awk '$1 == "up" { print $3; exit }' "$RT/box.rep")
	sleep 1.5
	d2=$(awk '$1 == "up" { print $3; exit }' "$RT/box.rep")
	[ -n "$d2" ] || {
		echo "FAIL ($what): the emulator wrote no report, so nothing can be said about"
		echo "      what the daemon sent"; tail -3 "$RT/box.log"; return 1; }
	[ "$d2" -gt "$((${d1:-0} + 1000))" ] || {
		echo "FAIL ($what): the segment is receiving again but unicast only"
		echo "      $((d2 - ${d1:-0})) upstream frames in 1.5 s — the box's outputs are dead"
		tail -n "+$floor" "$LOG" | tail -20; return 1; }
	echo "MEASURED ($what): rx.frames_ok $ok1 -> $ok2 over 2.5 s; upstream sent"
	echo "          $((d2 - ${d1:-0})) frames in 1.5 s; props $P"
	return 0
}

check_joined "first join" 1 || exit 1

# ---- DROP AND REJOIN, TWICE. --------------------------------------------------------
for CYCLE in 1 2; do
	DROPS_BEFORE=$(grep -c "\[rej0\] segment dropped" "$LOG")
	UPS_BEFORE=$(grep -c "\[rej0\] segment up" "$LOG")
	ip link set rej0 down
	for ((i = 0; i < 60; i++)); do
		[ "$(grep -c "\[rej0\] segment dropped" "$LOG")" -gt "$DROPS_BEFORE" ] && break
		sleep 0.25
	done
	[ "$(grep -c "\[rej0\] segment dropped" "$LOG")" -gt "$DROPS_BEFORE" ] || {
		echo "FAIL (cycle $CYCLE): the link went down past the hold and the segment never dropped"
		tail -20 "$LOG"; exit 1; }
	# EVERYTHING BELOW IS READ FROM HERE ON. The previous listener's healthy counters are
	# above this line and can no longer be mistaken for this one's.
	FLOOR=$(( $(wc -l < "$LOG") + 1 ))
	ip link set rej0 up
	for ((i = 0; i < 80; i++)); do
		[ "$(grep -c "\[rej0\] segment up" "$LOG")" -gt "$UPS_BEFORE" ] && break
		sleep 0.25
	done
	[ "$(grep -c "\[rej0\] segment up" "$LOG")" -gt "$UPS_BEFORE" ] || {
		echo "FAIL (cycle $CYCLE): the link came back and the segment was never served again"
		tail -n "+$FLOOR" "$LOG" | tail -20; exit 1; }
	check_joined "rejoin $CYCLE" "$FLOOR" || exit 1
done

kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
echo "PASS: a box-master join survives two drop/rejoin cycles carrying audio"
INNER
)
rc=$?
echo "$OUT"
[ $rc -eq 77 ] && exit 77
[ $rc -ne 0 ] && exit $rc
echo "$OUT" | grep -q "^PASS:" || { echo "FAIL: the inner namespace produced no verdict"; exit 1; }
exit 0
