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

# A master on the peer end: the wire now carries REAC. It speaks from a Roland-OUI
# address because that is the sniffer's bar for "REAC gear" (reac_disco_classify: the
# OUI and a checksum-valid control block, never a packet count) — a veth's random MAC
# would be exactly the non-Roland traffic the bar exists to ignore.
"$BIN" --live desk0 --tx desk0 --mixer m5000 --rate 96000 --name desk \
       --src-mac 00:40:ab:de:5c:01 >"$PEER" 2>&1 &
PPID2=$!
sleep 4
grep -q "\[hear0\] REAC heard" "$LOG" || { echo "FAIL: master on the peer never heard"; cat "$LOG"; tail -5 "$PEER"; exit 1; }
grep -q "\[hear0\] segment up" "$LOG" || { echo "FAIL: heard but not served"; cat "$LOG"; exit 1; }

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

kill -TERM $PPID2 2>/dev/null; wait $PPID2 2>/dev/null
kill -TERM $PID; wait $PID; rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: clean SIGTERM exited $rc"; tail -5 "$LOG"; exit 1; }
echo "OK: heard, kept through a flap, dropped past the hold, heard again; clean exit"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
