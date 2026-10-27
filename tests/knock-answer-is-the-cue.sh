#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# THE KNOCK'S ANSWER IS THE MASTER'S CUE, AND THE ANSWER IS GRANTED AS IT IS HANDED OVER (#58).
#
# Rig, 2026-10-27: reac-pw 1.0.39 lost an S-1608 and an S-4000S, straight on two NICs, on
# every cycle. Both boxes answered the knock within a second; the daemon then (1) sat out the
# hunt's three-cadence window on top of that answer, so the master's first frame left 3.0 s
# after the box's, and (2) held the box's JOIN until a push of its own had gone out whole,
# granting 0.69 s later. A knocked S-4000S holds 2 s; the S-1608 left 1.2 s after
# ESTABLISHED.
#
# The far end is libreac's fake_box: a LINKED, SILENT box that transmits nothing until a
# master puts a COMPLETE scene transfer on the wire, then answers it with its commit report
# — the rig's boxes exactly, and the opposite of this repo's slave peer, which floods on
# start and is taken by the box-spoke-first road before any knock is answered. The box is
# started BEFORE the daemon, as the rig's boxes were powered before it.
#
# Three bars, each red on 1.0.39 and green since:
#   - the master takes the wire within 2 s of ANSWERED (1.0.39: 3 s), by the knock's road;
#   - the carried answer takes the edge as it is ingested: "PROBING -> GRANTING (rx CONFIG)"
#     (1.0.39: "(timer)", the hold for its own push);
#   - ESTABLISHED follows.
# fake_box lives in the other repo, so the path is a knob; without it the test SKIPS.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
FAKE="${2:-}"
SKIP=77
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user \
	bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"
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

ip link add cue0 type veth peer name cueb0 || exit 90
ip link set cueb0 netns $NSPID || exit 90
ip link set cue0 up; $in_peer ip link set cueb0 up

# The box first: silent, linked, waiting for a whole push.
$in_peer "$FAKE" cueb0 40 >"$RT/box.log" 2>&1 &
FAKEPID=$!
sleep 1

# The hearing end: no flags, an EMPTY home — nothing declares the interface or pins a role,
# so cue0 is hunted, and a silent wire is knocked on.
HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

wait_for() {   # wait_for <pattern> <secs>: polls 5x a second
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -q "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}
fail() { echo "FAIL: $*"; echo "--- daemon (cue0) ---"; grep -E "\[cue0\]|reac-master" "$LOG" | tail -30;
         echo "--- box ---"; tail -5 "$RT/box.log"; exit 1; }

wait_for "\[cue0\] nothing heard in .* KNOCKING" 10 || fail "a silent wire was never knocked on"
wait_for "\[cue0\] ANSWERED" 20 || fail "the box never answered the knock (fake_box answers a COMPLETE push)"
# BAR 1: the master takes the wire on the answer. 1.0.39 said "no master heard in 3 s and a
# box is present (it answered the knock)" three seconds after this line; the 2 s wait is
# red there and green at the next 200 ms poll.
wait_for "\[cue0\] the wire carried nothing for .* before the knock and a box answered it; taking the wire as MASTER now" 2 ||
	fail "the box answered the knock and the master did not take the wire within 2 s (the hunt's window ran on top of the answer)"
# BAR 2: the answer the knock carried takes the edge as it is ingested, not after a second
# push of the scene the box already committed.
wait_for "PROBING -> GRANTING (rx CONFIG)" 3 ||
	fail "the carried answer did not take the edge (a '(timer)' transition means the master held it for a push of its own)"
# BAR 3: and it establishes.
wait_for "GRANTING -> ESTABLISHED" 10 || fail "granted on the answer and never established"
grep -q "PROBING -> GRANTING (timer)" "$LOG" && fail "a '(timer)' edge out of PROBING was taken as well: the hold ran"
echo "ok: knocked, answered, taken within 2 s, granted on the carried answer, established"
kill -TERM $PID 2>/dev/null; sleep 0.3
INNER
)
RC=$?
case "$OUT" in *"SKIP: "*) echo "${OUT##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
printf '%s\n' "$OUT"
exit $RC
