#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A `role = box` SEGMENT IS NOT RE-DECIDED WHEN THE MIXER IS QUIET.
# (docs/design/specs/2026-09-17-the-daemon-can-be-a-box.md §5; main.c hearing_reevaluate.)
#
# A stagebox that only exists once a desk is powered is not a stagebox. A box's wire is
# SILENT for as long as nobody has switched the mixer on, and that silence says nothing
# about what the segment is — the operator wrote `role = box` in reac-pw.conf and that is
# the whole decision. hearing_reevaluate's exemption was written for the pinned recorder
# (`role = slave`) and keyed on that intent alone, so a box fell through it and was torn
# down and re-heard every ten seconds for as long as it waited: its declaration dropped,
# its published pair rebuilt around the gap, for ever.
#
# TWO SEGMENTS, ONE DAEMON, AND THE SECOND IS THE POSITIVE CONTROL. An absence measured by
# a probe that has never seen a presence is not a measurement:
#   nbx0  `role = box, model = s1608` on a wire with NOBODY on it — the claim. No
#         re-decision line may ever appear for it.
#   ngn0  a fake box master that JOINS and then STOPS — the control, and the same thing
#         tests/master-gone-is-re-decided.sh proves on its own wire (#97). Its re-decision
#         line MUST appear, in the same run, in the same log, read by the same grep. If it
#         does not, this file has measured nothing about nbx0 either and says so.
#
# Isolation as tests/master-gone-is-re-decided.sh: an unprivileged user+net+pid namespace
# with its own PipeWire on a private runtime dir, the peer ends in a nested network
# namespace. Skips (77) where the namespaces, iproute2 or PipeWire are unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
BIN=$(readlink -f "$BIN"); FAKE=$(readlink -f "$FAKE")
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
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

LINE0() { echo $(( $(wc -l < "$LOG") + 1 )); }
wait_for_since() {   # wait_for_since <first-line> <pattern> <secs>
	local floor="$1" pat="$2" secs="$3" i
	for ((i = 0; i < secs * 5; i++)); do
		tail -n "+$floor" "$LOG" | grep -q "$pat" && return 0
		sleep 0.2
	done
	return 1
}

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer ends"; exit 77; }
in_peer="nsenter -t $NSPID -n"

for pair in nbx0:nbxp0 ngn0:ngnp0; do
	ip link add "${pair%%:*}" type veth peer name "${pair##*:}" || exit 90
	ip link set "${pair##*:}" netns $NSPID || exit 90
done

# THE BOX'S ROW, WRITTEN WHERE A BOX ROLE CAN COME FROM AT ALL: reac-pw.conf. The control
# segment is left to the hunt, which is what elects it a slave when the box master floods.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment nbx0]\nrole = box\nmodel = s1608\n' > "$CONF/.config/reac-pw/reac-pw.conf"

BOXMAC=00:40:ab:c4:dc:a2
$in_peer "$FAKE" ngnp0 "$BOXMAC" 8 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
FAKEPID=$!
sleep 0.5
ip link set nbx0 up; ip link set ngn0 up
$in_peer ip link set nbxp0 up; $in_peer ip link set ngnp0 up
# NOTHING EVER SPEAKS ON nbxp0. It is up so the box segment is served; the far end is a
# mixer that has not been switched on, which is the case being measured.

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

# ---- 1. BOTH SEGMENTS ARE REALLY UP. Neither claim below means anything otherwise.
wait_for_since 1 "\[nbx0\] role = box —" 25 || {
	echo "FAIL: the box row was never taken up — nothing was measured"
	tail -25 "$LOG"; exit 1; }
wait_for_since 1 "\[ngn0\] box masters this wire" 30 || {
	echo "FAIL: the control wire's box master was never joined — the control cannot fire"
	tail -25 "$LOG"; tail -3 "$RT/box.log"; exit 1; }

# ---- 2. THE CONTROL'S MASTER STOPS, link left up (a box switched to S and rebooted).
MARK=$(LINE0)
kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null

# ---- 3. THE CONTROL IS RE-DECIDED. Bounded at ~10 s by the heard latch plus the same
# again; 60 s is generous on a loaded machine. This is the PRESENCE that makes the absence
# below a measurement: the ladder runs, the line exists, and this grep finds it.
wait_for_since "$MARK" "\[ngn0\] the master this segment was following has been silent" 60 || {
	echo "FAIL: the control segment was NOT re-decided when its master left, so this run"
	echo "      cannot testify about the box segment either (#97 is the other test)"
	tail -n "+$MARK" "$LOG" | tail -25; exit 1; }

# ---- 4. THE CLAIM. The box has been on a silent wire since the daemon started — longer
# than the control waited — and it must never have been re-heard, not once.
if grep -q "\[nbx0\] the master this segment was following has been silent" "$LOG"; then
	echo "FAIL: the box segment was re-decided while its mixer was merely quiet — a box"
	echo "      waiting for a desk to be powered is torn down every ten seconds"
	grep -n "\[nbx0\]" "$LOG" | tail -20; exit 1
fi
if grep -q "\[nbx0\] .*re-hearing the wire" "$LOG"; then
	echo "FAIL: the box segment was re-heard"; grep -n "\[nbx0\]" "$LOG" | tail -20; exit 1
fi
# AND IT IS STILL THE BOX IT WAS ASKED TO BE: one BOX role line, never a second one from
# a rebuild. A segment re-served after a drop says it again.
N=$(grep -c "\[nbx0\] role = box —" "$LOG")
[ "$N" = "1" ] || { echo "FAIL: the box row was taken up $N times — it was re-served"
                    grep -n "\[nbx0\]" "$LOG" | tail -20; exit 1; }

echo "OK: a quiet mixer does not re-decide a box segment (1 `role = box` line, no re-hearing)"
echo "    and the control segment on the same daemon WAS re-decided when its master left"
INNER
)
rc=$?
echo "$OUT"
exit $rc
