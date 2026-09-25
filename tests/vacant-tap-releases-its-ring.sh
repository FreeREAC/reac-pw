#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, audit 2026-09-24 M6: A VACANT TAP DOOR GIVES BACK THE RX RING IT ALLOCATED.
#
# A segment pinned `tap` on a wire with nothing on it falls through listener_open into the
# door (door_vacant), and reac_rx_open has already opened an AF_PACKET socket and allocated
# the ring by then -- for a door that never starts RX and publishes nothing. Its teardown is
# listener_close's tap branch, which closed the tap's own feeders and dropped the nodes but
# never closed L->rx or freed L->ring, and hearing_serve's memset then lost both pointers.
# A pinned tap on an intermittent mirror port leaked one socket and one ring every time
# its door was dropped and re-opened.
#
# WHAT LEAKED, MEASURED: the ring. The audit named the RX socket too, but reac_rx_open
# opens its capture only to detect the rate and closes it before returning (libreac
# 1.5.0, transport/src/reac_rx.c), so a door that never starts RX holds no RX fd. Its
# ring it does hold: 40 channels x pow2(rate/4) floats, megabytes, per teardown.
#
# THE SHAPE. One silent veth, pinned `tap` in reac-pw.conf. The link is held down past
# the ifscan hold five times; each drops the segment and re-serves it as a vacant door.
# The daemon's open fds and VmData are sampled as each door is announced.
#
# PRESENCE FIRST: the door has to be announced every time, or a flat count is about a
# segment that never re-opened.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }
BIN=$(readlink -f "$BIN")

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

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n"
ip link add vtap0 type veth peer name pvtap0 || exit 90
ip link set pvtap0 netns $NSPID || exit 90
$in_peer ip link set pvtap0 up || exit 90

mkdir -p "$CONF/.config/reac-pw"
printf '[segment vtap0]\nrole = tap\n' > "$CONF/.config/reac-pw/reac-pw.conf"

HOME="$CONF" "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 1
ip link set vtap0 up

doors() { grep -c "\[vtap0\] TAP with nothing to serve" "$LOG"; }
wait_doors() {   # wait_doors <n>
	local i
	for ((i = 0; i < 150; i++)); do
		kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -15 "$LOG"; exit 91; }
		[ "$(doors)" -ge "$1" ] && return 0
		sleep 0.2
	done
	return 1
}
for n in 1 2 3 4 5 6; do
	wait_doors $n || { echo "door-missing $n"; break; }
	sleep 0.5
	echo "sample $n fds $(ls /proc/$PID/fd | wc -l) vmdata_kb $(awk '$1 == "VmData:" { print $2 }' /proc/$PID/status)"
	[ $n -lt 6 ] || break
	ip link set vtap0 down; sleep 4.5; ip link set vtap0 up   # past REAC_IFSCAN_DOWN_HOLD_NS (3 s)
done
echo "doors $(doors)"
grep -a -E "\[vtap0\]" "$LOG" | grep -a -E "dropped|TAP with nothing" | head -3 | sed 's/^/  log /'
kill -0 $PID 2>/dev/null || { echo "daemon-died at the end"; tail -15 "$LOG"; exit 91; }
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
)
rc=$?
echo "$OUT" | sed 's/^/  /'
fail() { echo "FAIL: $1"; exit 1; }
echo "$OUT" | grep -qa 'daemon-died' && fail "the daemon died while its vacant tap door was being bounced"
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || fail "the namespace body exited rc=$rc"

# 0. THE DOOR RE-OPENED EVERY TIME.
D=$(echo "$OUT" | awk '$1 == "doors" { print $2; exit }')
[ "${D:-0}" -ge 6 ] || fail "the vacant tap door was announced ${D:-0} time(s), not 6 — the bounce never re-opened it, so nothing below is about M6"

# 1. WHAT THE SECOND DOOR HELD, THE SIXTH HOLDS. The first sample is skipped: the first
#    open also starts what lives for the daemon's life (PipeWire's connection, the hunt).
read -r F2 V2 < <(echo "$OUT" | awk '$1 == "sample" && $2 == 2 { print $4, $6 }')
read -r F6 V6 < <(echo "$OUT" | awk '$1 == "sample" && $2 == 6 { print $4, $6 }')
[ -n "${V2:-}" ] && [ -n "${V6:-}" ] || fail "no samples at the 2nd and 6th door"
[ "$F6" -le "$F2" ] || fail "open fds grew from $F2 to $F6 over four vacant-door teardowns"
# ONE RING IS MEGABYTES (40 channels x pow2(rate/4) floats: 2.5 MB at 48 kHz), and a
# calloc that size is its own mapping, so it shows in VmData whether or not it is touched.
# 1 MB of slack over four teardowns is less than half of one leaked ring.
[ $((V6 - V2)) -lt 1024 ] || fail "VmData grew by $((V6 - V2)) kB over four vacant-door teardowns — each one leaves its RX ring allocated"
echo "OK: four vacant tap doors torn down and re-opened left fds ($F2 -> $F6) and VmData ($V2 -> $V6 kB) flat"
exit 0
