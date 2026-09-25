#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# REAL SOCKETS: an ungranted slave gets OFF THE WIRE, measured from the other end.
#
# The defect (reac-captures 85c1e97, m200-master-441k-2026-09-11): reac-pw's slave courted a
# live M-200 for 93 s at 100 % duty without ever being granted. The desk keeps ONE box
# session per segment and any upstream of the enrolled geometry feeds its liveness, so our
# stream held the slot; the desk emitted zero scene transfers and the real S-1608 rebooting
# beside us never enrolled. With our slave absent the same desk released the slot 7.148 s
# after its box went quiet. libreac now bounds the courtship at 4 s and then emits NOTHING
# for 10 s (libreac docs/design/specs/2026-09-12-bounded-ungranted-courtship.md).
#
# libreac's tests/test_link.c drives the PURE FSM. This drives the real engine —
# reac_slave_open/reac_slave_start on an AF_PACKET socket — against a master that announces
# at wire rate and never grants, and reads the answer OFF THE WIRE at the far end of a veth
# pair. Asking the slave what it decided would be asking the accused.
#
# PRESENCE BEFORE ABSENCE: the first assertion is that the ear HEARD the slave at all, and
# the second is that the burst it heard is a real courtship. Only then is a silence read as
# a silence rather than as a deaf probe.
#
# The peer end lives in a NESTED network namespace (tests/hearing-finds-a-segment.sh's header
# has the reasoning: a veth with both ends here makes every question unanswerable). Nothing
# leaves the namespace, so it cannot perturb a live REAC segment. Skips (77) where the
# namespaces are unavailable, and where the master could not hold its pacing — a measurement
# that could not be taken is not a pass.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/courtship-probe}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v ip      >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

# 44.1 k is the capture's rate: fps = 44100 / 12 = 3675, and both bounds scale with it.
FPS=$FACT_PKT_RATE_44K1
RUN=40
SLAVE_MAC=00:40:ab:9f:9e:be

OUT=$(unshare -r -n --map-root-user bash -s -- "$BIN" "$FPS" "$RUN" "$SLAVE_MAC" <<'INNER'
set -u
BIN="$1"; FPS="$2"; RUN="$3"; SLAVE_MAC="$4"
RT=$(mktemp -d)
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$RT"; }
trap cleanup EXIT

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }

ip link add cts0 type veth peer name ctm0 || exit 90
ip link set ctm0 netns $NSPID || exit 90
ip link set cts0 up
nsenter -t $NSPID -n ip link set ctm0 up

# The master first, so the wire is never silent under the slave: a slave that never learns a
# master never leaves the flood, and the budget under test is the COLD-CONNECT's.
nsenter -t $NSPID -n "$BIN" master ctm0 "$SLAVE_MAC" "$FPS" "$RUN" >"$RT/master.out" 2>"$RT/master.err" &
MPID=$!
sleep 0.5
"$BIN" slave cts0 "$SLAVE_MAC" "$FACT_SAMPLE_RATE_44K1" $((RUN - 2)) >"$RT/slave.out" 2>"$RT/slave.err" &
SPID=$!

wait $MPID; mrc=$?
kill -TERM $SPID 2>/dev/null; wait $SPID 2>/dev/null
# OUR OWN PROBE AND OUR OWN ENGINE FAILING ARE FAILS, NOT SKIPS (audit 2026-09-24, H3):
# this namespace's root may open AF_PACKET, so nothing about the machine explains either.
[ $mrc -eq 0 ] || { echo "FAIL: the master end exited rc=$mrc"; tail -3 "$RT/master.err"; exit 94; }
grep -q "reac_slave_open failed\|AF_PACKET" "$RT/slave.err" && {
	echo "FAIL: the slave engine could not open its socket"; tail -3 "$RT/slave.err"; exit 94; }
cat "$RT/master.out"
grep -a 'STATE' "$RT/slave.err" | sed 's/^/slave: /'
exit 0
INNER
)
rc=$?
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }

echo "$OUT" | sed 's/^/  /'

val() { echo "$OUT" | awk -v k="$1" '$1 == k { print $2; f = 1; exit } END { exit !f }'; }
fail() { echo "FAIL: $1"; exit 1; }

HEARD=$(val heard)       || fail "the master printed no census at all"
ACHIEVED=$(val achieved_fps) || fail "no achieved_fps in the census"
BURST=$(val first_burst_s)   || fail "no first_burst_s in the census"
SILENCE=$(val longest_silence_s) || fail "no longest_silence_s in the census"
BURSTS=$(val bursts)     || fail "no burst count in the census"

# 0. THE MEASUREMENT WAS TAKEN. A master that could not hold ~FPS turns every wall-clock
#    number below into a number about the scheduler, not about the slave: the FSM's bounds
#    are counted in frame periods and scale with the rate its peer actually sends at.
awk -v a="$ACHIEVED" -v f="$FPS" 'BEGIN { exit !(a > f * 0.85 && a < f * 1.15) }' || {
	echo "SKIP: the master paced $ACHIEVED fps against a target of $FPS — the wall-clock"
	echo "      bounds cannot be read off a wire whose rate moved"; exit $SKIP; }

# 1. PRESENCE. The ear heard a courtship — without this every silence below is a deaf probe.
[ "$HEARD" -gt 5000 ] || fail "the master heard only $HEARD frames from the slave; a courtship is thousands"

# 2. THE BURST IS BOUNDED. The bounded presence flood is 5460 frames (1.486 s at $FPS) and
#    the cold-connect budget is 4 s, so a courtship that ends is one that ends by ~5.5 s.
#    On the unpatched engine there is no gap at all and first_burst_s reads -1.
awk -v b="$BURST" 'BEGIN { exit !(b > 0) }' \
	|| fail "the slave never stopped transmitting (first_burst_s=$BURST) — this IS the defect"
awk -v b="$BURST" 'BEGIN { exit !(b <= 7.0) }' \
	|| fail "the first burst ran $BURST s; the flood (1.486 s) plus the 4 s budget is ~5.5 s"

# 3. AND THE SILENCE OUTLASTS THE DESK'S SESSION HOLD — the whole point of the number.
awk -v s="$SILENCE" 'BEGIN { exit !(s >= 9.5) }' \
	|| fail "the longest silence was $SILENCE s; the backoff is 10 s"
awk -v s="$SILENCE" 'BEGIN { exit !(s > 7.148) }' \
	|| fail "the longest silence ($SILENCE s) does not outlast the M-200's measured 7.148 s hold"

# 4. AND IT COURTS AGAIN. A slave that went quiet for good would pass 2 and 3 and be a
#    different defect: the backoff is a duty cycle, not a give-up.
[ "$BURSTS" -ge 2 ] || fail "only $BURSTS burst — the slave never re-flooded after the backoff"

echo "MEASURED: first burst ${BURST}s, longest silence ${SILENCE}s over ${BURSTS} bursts," \
     "$HEARD frames heard at ${ACHIEVED} fps"
echo "OK: an ungranted slave on a real socket courts for a bounded time and then leaves the" \
     "wire for longer than the desk holds its box session"
