#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a daemon started with NOTHING finds a segment by hearing it, keeps it
# through a link flap, drops it when the link stays down, and hears it again afterwards
# (openmixer's trunk-VLAN spec, amendment 2026-09-02 §7/§8).
#
# test_reac_ifscan proves the table. This proves the lines that JOIN it to the sockets
# and the listeners: the netlink fd on the main loop, the sniffer's classify, the serve
# from the 200 ms poll, the drop from the hold. A veth pair stands in for the cable: the
# hearing daemon runs bare, a second reac-pw runs as a MASTER on the peer end, and the
# peer's admin state drives the carrier the hearing end sees.
#
# PRESENCE BEFORE ABSENCE: "heard" and "segment up" are asserted before any quiet claim.
#
# Runs entirely inside an unprivileged user+net namespace, so it cannot perturb a live
# REAC segment. Skips (77) where the namespace, iproute2 or PipeWire is unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
LOG=$(mktemp); PEER=$(mktemp); CONF=$(mktemp -d)
trap 'rm -rf "$LOG" "$PEER" "$CONF"' EXIT

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

ip link add hear0 type veth peer name desk0 || exit 90
ip link set hear0 up
ip link set desk0 up

# The hearing end: no flags, an EMPTY home — nothing declares an interface.
HOME="$CONF" "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || {
	echo "hearing daemon never got running — no PipeWire in this namespace?"
	tail -3 "$LOG"
	exit 77
}
grep -q "hearing: .* Ethernet interface" "$LOG" || { echo "FAIL: no hearing banner"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] link up — listening for REAC" "$LOG" || { echo "FAIL: hear0 not sniffed"; cat "$LOG"; exit 1; }
if grep -q "\[hear0\] segment up" "$LOG"; then
	echo "FAIL: hear0 became a segment before anything was heard"; cat "$LOG"; exit 1
fi

# A master on the peer end: the wire now carries REAC. The sniffer's bar for "REAC gear"
# is the PROTOCOL FRAME and nothing else (reac_disco_classify: a 0x8819 frame whose
# control block verifies, or a filler — never a packet count, and since 2026-09-03 never
# a MAC's vendor prefix either). The source address below is a real Roland one only
# because it is what this rig's captures carry; the classifier would take any.
"$BIN" --live desk0 --tx desk0 --mixer m5000 --rate 96000 --name desk \
       --src-mac 00:40:ab:de:5c:01 >"$PEER" 2>&1 &
PPID2=$!
sleep 4
grep -q "\[hear0\] REAC heard" "$LOG" || { echo "FAIL: master on the peer never heard"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
grep -q "\[hear0\] segment up" "$LOG" || { echo "FAIL: heard but not served"; cat "$LOG"; exit 1; }
# AND ON THE RIGHT END OF THE PAIRING. A desk masters this wire, so the daemon joins it as
# a SLAVE and follows its pace (trunk-VLAN spec S7 step 4, arbitration S2). Nothing was
# configured to say so; the verdict came from the frames.
grep -q "\[hear0\] a desk masters this segment" "$LOG" || {
	echo "FAIL: a desk was mastering the wire and the hunt did not say so"; cat "$LOG"; exit 1; }
grep -q "\[hear0\] segment up (slave, chosen by hearing the wire)" "$LOG" || {
	echo "FAIL: served, but not as the slave the wire called for"; cat "$LOG"; exit 1; }

# A flap shorter than the hold: the segment is kept, nothing rebuilt.
ip link set desk0 down; sleep 1
ip link set desk0 up;   sleep 2
grep -q "\[hear0\] link back inside the hold — segment kept" "$LOG" || {
	echo "FAIL: flap did not read as kept"; cat "$LOG"; exit 1; }
if [ "$(grep -c "\[hear0\] segment dropped" "$LOG")" -ne 0 ]; then
	echo "FAIL: a flap inside the hold dropped the segment"; cat "$LOG"; exit 1
fi

# Link down past the hold: the segment drops, and the interface is sniffed again when
# link returns, so the master still on the peer is heard afresh.
ip link set desk0 down; sleep 4.5
grep -q "\[hear0\] segment dropped" "$LOG" || { echo "FAIL: no drop after the hold"; cat "$LOG"; exit 1; }
ip link set desk0 up; sleep 5
[ "$(grep -c "\[hear0\] link up — listening for REAC" "$LOG")" -ge 2 ] || {
	echo "FAIL: not sniffed again after the drop"; cat "$LOG"; exit 1; }
[ "$(grep -c "\[hear0\] segment up" "$LOG")" -ge 2 ] || {
	echo "FAIL: not served again after the drop"; cat "$LOG"; exit 1; }

# ---- THE VACANT WIRE, WHICH IS THE 2026-09-08 OUTAGE IN MINIATURE. The desk goes away
# and a BOX takes its place: a daemon that was told nothing, and that has just been
# slaving to a desk, must now DRIVE the segment — hunt, grant, establish. On the rig two
# boxes sat ungranted for exactly this hole, with a `REAC_ROLE=slave` floor in a file.
kill -TERM $PPID2 2>/dev/null; wait $PPID2 2>/dev/null
ip link set desk0 down; sleep 4.5        # past the hold: the segment closes, sniffing resumes
ip link set desk0 up;   sleep 1
"$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] no master heard in" 15 || {
	echo "FAIL: a box on a vacant wire and the daemon never took it"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
wait_for "\[hear0\] segment up (master, chosen by hearing the wire)" 10 || {
	echo "FAIL: the wire was taken but the segment did not come up as master"; cat "$LOG"; exit 1; }
# The job, not the decision: the box it heard is ENROLLED. A role elected and nothing
# granted would be the same silence the outage had.
wait_for "reac-master: .* -> ESTABLISHED" 20 || {
	echo "FAIL: took the wire as master but never established with the box"
	tail -20 "$LOG"; tail -5 "$PEER"; exit 1; }
# ---- A PIN IS SERVED WITHOUT A HUNT. `REAC_ROLE_<segment>` is an answer about this
# wire (arbitration S8a: the role is a setting), so it waits only for the wire to BE a
# segment. The box stays where it is; only the conf changes, and the segment is bounced
# so the sniffer re-reads it.
mkdir -p "$CONF/.config/reac-pw"
echo "REAC_ROLE_hear0=master" > "$CONF/.config/reac-pw/reac-pw.env"
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null
ip link set desk0 down; sleep 4.5      # both ends of a veth lose carrier together
ip link set desk0 up;   sleep 1
"$BIN" --live desk0 --tx desk0 --role slave --box-channels 16 --name box \
       --src-mac 00:40:ab:c4:80:41 >"$PEER" 2>&1 &
BOXPID=$!
wait_for "\[hear0\] REAC heard, and REAC_ROLE_hear0 pins this segment as MASTER" 15 || {
	echo "FAIL: the per-segment pin was not honoured on the first classifying frame"
	tail -20 "$LOG"; exit 1; }
wait_for "\[hear0\] segment up (master, pinned by REAC_ROLE_<segment>)" 10 || {
	echo "FAIL: served, but not reported as pinned"; tail -20 "$LOG"; exit 1; }
kill -TERM $BOXPID 2>/dev/null; wait $BOXPID 2>/dev/null

kill -TERM $PID; wait $PID; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: clean SIGTERM exited $rc"; tail -5 "$LOG"; exit 1; }
echo "OK: heard, joined a desk as slave, kept through a flap, dropped past the hold,\n    heard again, took a vacant wire as master and established with the box, and\n    served a per-segment pin without a hunt"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
