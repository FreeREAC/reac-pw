#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# INTEGRATION (#95): the WHOLE DAEMON reacts to a cable, and says why.
#
# The unit tests prove the watcher reports edges and the pacer re-establishes when asked.
# Neither proves the six lines that JOIN them — the netlink fd on the main loop, the callback,
# the request across to the pacer thread. That join is exactly the kind of path a green suite
# hides: every part works, nothing calls anything.
#
# So this drives the real binary. It toggles a real interface and requires two things the
# daemon can only produce by actually having reacted:
#
#   1. the operator line naming the edge, and
#   2. the FSM transcript ATTRIBUTING a transition to the cable — `-> IDLE (LINK-UP on the
#      wire)`. Attribution is the half that matters: before #95 a rate change printed `rx ?`
#      here, and a cable fault reported as anything else sends the operator to the wrong end
#      of the room.
#
# PRESENCE BEFORE ABSENCE: the DOWN edge is asserted first, so a run that saw nothing fails
# loudly instead of passing an "and no spurious edges" check by being deaf.
#
# Runs entirely inside an unprivileged user+net namespace on its own dummy interface, so it
# cannot perturb a live REAC segment. Skips (77) where the namespace or PipeWire is
# unavailable.
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
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

# <= 15 chars: IFNAMSIZ is 16 and a longer name is refused by the watch's length guard
# before it reaches anything, so a test using one would prove nothing about the watch.
ip link add reac-t0 type dummy || exit 90
ip link set reac-t0 up

"$BIN" --live reac-t0 --tx reac-t0 --mixer m5000 --rate 96000 >"$LOG" 2>&1 &
PID=$!
sleep 5
kill -0 $PID 2>/dev/null || {
	echo "daemon never got running — no PipeWire in this namespace?"
	tail -3 "$LOG"
	exit 77
}

# Far enough apart to be two transitions: a flap that resolves inside one drain is
# deliberately collapsed to nothing, which is a different (tested) behaviour.
ip link set reac-t0 down; sleep 1
ip link set reac-t0 up;   sleep 2

kill -TERM $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null

grep -aE 'LINK-(UP|DOWN)' "$LOG"
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"; echo "$OUT" | sed 's/^/  /'; exit $SKIP; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }

# 1. THE DAEMON NOTICED AT ALL. This is the assertion that fails on the pre-#95 binary,
#    which logged absolutely nothing for either edge on either NIC family.
echo "$OUT" | grep -q 'reac-master: LINK-DOWN on reac-t0' \
	|| fail "no LINK-DOWN line — the daemon did not notice the cable go, which IS #95"
echo "$OUT" | grep -q 'reac-master: LINK-UP on reac-t0' \
	|| fail "no LINK-UP line — the daemon did not notice the cable return, which IS #95"

# 2. AND IT RE-ESTABLISHED, ATTRIBUTED TO THE CABLE. The transition is what makes the
#    notice more than a log line: without it the master keeps a stale establishment and a
#    box that went quiet still needs a daemon restart.
echo "$OUT" | grep -q -- '-> IDLE (LINK-DOWN on the wire)' \
	|| fail "the LINK-DOWN was logged but drove no re-establish"
echo "$OUT" | grep -q -- '-> IDLE (LINK-UP on the wire)' \
	|| fail "the LINK-UP was logged but drove no re-establish"

echo "OK: a real cable down/up on a real interface makes the whole daemon re-establish, "\
"twice, with the transcript naming the WIRE as the reason"
