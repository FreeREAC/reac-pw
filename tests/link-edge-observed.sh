#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# INTEGRATION (#95): does the watch actually SEE a carrier change?
#
# The unit test feeds bytes this repo wrote. A parser that understood none of them would
# pass every synthetic case by doing nothing, and a socket that joined no multicast group
# would look identical to a quiet wire. So this drives REAL `ip link` toggles and requires
# the watcher to report them, in order, from the kernel's own messages.
#
# It is the half of #95 that had no evidence at all: on the rig the kernel logged
# `Link status is: 0` then `1` for every box power-cycle and reac-pw logged nothing, on a
# USB AX88179 and a PCIe r8169 alike. Nothing was looking.
#
# PRESENCE BEFORE ABSENCE. The first assertion is that the UP edge IS seen; only then does
# the count assertion mean anything. A test that only checked "no spurious edges" would pass
# on a watcher that reports nothing at all — which is exactly the bug.
#
# Runs entirely inside an unprivileged user+net namespace on its own dummy interface, so it
# cannot perturb a live REAC segment. Skips (77) where that namespace is unavailable.
set -u
BIN="${1:?usage: $0 /path/to/test_reac_linkmon}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without the
# dummy driver is a machine this test cannot run on, and says so here, so a later `|| exit 90`
# is a FAIL.
unshare -r -n sh -c 'ip link add d0 type dummy' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a dummy link in a namespace (no dummy driver)"; exit $SKIP; }

OUT=$(unshare -r -n --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
# A dummy device's carrier follows its admin state, so `up`/`down` produce exactly the
# RTM_NEWLINK carrier transitions a cable produces on a real NIC. Named <= 15 chars: IFNAMSIZ
# is 16, and a longer name is refused by the watch's length guard before it reaches anything.
ip link add reac-t0 type dummy || exit 90

"$BIN" --watch reac-t0 3000 &
PID=$!
sleep 0.5                      # the watch is open and has seeded its carrier

ip link set reac-t0 up;   sleep 0.5
ip link set reac-t0 down; sleep 0.5
ip link set reac-t0 up;   sleep 0.5

wait $PID
INNER
)
rc=$?

echo "$OUT" | sed 's/^/  /'

# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }

EDGES=$(echo "$OUT" | grep '^EDGE ' | tr '\n' ' ' | sed 's/ $//')

# PRESENCE FIRST: the watch must see the carrier come up at all.
case "$EDGES" in
	*"EDGE UP"*) ;;
	*) echo "FAIL: no UP edge observed — the watch saw NOTHING, which is the #95 bug itself"
	   exit 1 ;;
esac

# Then the exact sequence: one edge per real transition, in order, no extras. A watcher that
# fired on every RTM_NEWLINK would report more than three here (the kernel sends several per
# `ip link set` — the admin flag and the carrier arrive as separate messages).
[ "$EDGES" = "EDGE UP EDGE DOWN EDGE UP" ] || {
	echo "FAIL: expected 'EDGE UP EDGE DOWN EDGE UP', got '$EDGES'"
	exit 1
}

echo "OK: a real carrier up/down/up on a real interface is reported as exactly three edges, "\
"in order, from the kernel's own RTM_NEWLINK messages"
