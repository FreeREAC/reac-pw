#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, END TO END: a box that DROPPED while its desk was away is re-acquired by
# the daemon alone, with nobody touching a cable.
#
# THE JOB, AND THE DAY IT WAS NOT DONE. 2026-09-16 on msi: a 77-minute s2idle, and on resume
# the daemon re-took the wire as MASTER and probed correctly for SEVENTY-THREE MINUTES across
# two processes — about 1620 completed scene pushes, 8003 frames a second leaving the NIC,
# rx_box_frames=0 — while an enrolled S-1608 sat there silent. Every unit test in this repo
# was green throughout. None of them could see it, because each one asks whether a part
# behaves and the part that had to behave was the daemon's willingness to act on its own
# silence. So this drives the REAL BINARY at the far end of a real veth and requires the
# operator's job: the box comes back, and nobody helped it.
#
# THE FAR END IS A BOX THAT CANNOT BE WOKEN BY FRAMES. libreac's fake_box under
# FAKE_BOX_DROPPED=1 receives whole transfers, counts them and answers NONE until it has
# watched its own carrier go and return — which is the S-1608's own decompiled FSM, "PHY
# LINK-UP (the only establish trigger; a data gap does NOT)" (reac-firmware-re
# REAC-PROTOCOL-FROM-SOURCE §10.2), and not this repo's opinion about it.
#
# THE POSITIVE CONTROL IS THE SECOND ARM, and without it the first proves nothing: the SAME
# binary, the same wire and the same emulator with the mode off enrols with NO EDGE AT ALL.
# A test that only ever saw the bounce arm could be passing because the daemon bounces
# everything, which would be a worse defect than the one it is here for.
#
# ISOLATION (tests/box-master-slave-join.sh's header carries the full reasoning): an
# unprivileged user+net+pid namespace with its own PipeWire on a private runtime dir, and
# the peer end of the veth in a NESTED network namespace, so the box really is another host.
# No wireplumber and no audio here — this proof is about enrolment, and a link that goes
# down and up is not a thing to measure a tone across.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
SKIP=77

[ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

# Long enough for the ladder's first edge (REAC_WAKE_GRACE_NS = 12 s) plus the box's
# flood, its cold-connect and our grant dwell, and short enough to be a test.
SECS="${REACPW_WAKE_SECS:-45}"

run_arm() {   # run_arm <dropped:0|1> ; prints the arm's transcript, exit 0 = the box enrolled
	FAKE_BOX_DROPPED="$1" unshare -r -n -m -p -f --mount-proc --map-root-user \
		bash -s -- "$BIN" "$FAKE" "$SECS" <<'INNER'
set -u
# SYSFS DOES NOT FOLLOW A NETWORK NAMESPACE, and the first run of this test is what
# proved it: the daemon read `carrier UNKNOWN on wke0` for the whole arm and REFUSED to
# bounce — correctly, because -1 is not "down" — while /sys still showed the host's
# interfaces, where no wke0 exists. A private sysfs per namespace is therefore part of
# the fixture, on BOTH sides: reac_link_carrier and fake_box both read
# /sys/class/net/<if>/carrier, which is the kernel's own answer and the reason neither
# needs a capability.
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; SECS="$3"
DROPPED="${FAKE_BOX_DROPPED:-0}"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# The peer end is another host, so "the box" is not our own netdev answering itself — and
# it gets its own sysfs for the reason above, because the box's whole behaviour in this
# proof turns on reading its own carrier.
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"

ip link add wke0 type veth peer name wkeb0 || exit 90
ip link set wkeb0 netns $NSPID || exit 90
ip link set wke0 up; $in_peer ip link set wkeb0 up

FAKE_BOX_DROPPED="$DROPPED" $in_peer "$FAKE" wkeb0 "$SECS" >"$RT/box.log" 2>&1 &
FAKEPID=$!

# One segment, named, at the rig's own rate: 96 kHz is 8000 fps, so a scene transfer
# completes every 2.6945 s and the ladder's three-push floor is reached at ~8.1 s, inside
# its 12 s grace. A hunt would reach the same MASTER role by the masterless licence (the
# path the live desk took at 14:38:56); this says it outright so the arm does not also
# depend on the hunt's timing.
HOME="$CONF" REAC_DEBUG=1 "$BIN" --live wke0 --tx wke0 --mixer m5000 --rate 96000 \
	--name wake >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || { echo "daemon exited early"; tail -5 "$LOG"; exit 91; }

for ((i = 0; i < SECS * 2; i++)); do
	grep -q "ESTABLISHED" "$LOG" && break
	sleep 0.5
done
sleep 1
kill -TERM $PID 2>/dev/null; sleep 0.5
wait $FAKEPID; BOXRC=$?

echo "--- box ---"; cat "$RT/box.log"
echo "--- daemon ---"; grep -E "makes the edge itself|is back up|did not answer|PROBING|GRANTING|ESTABLISHED|COULD NOT|REFUSED" "$LOG" | tail -25
echo "BOXRC=$BOXRC"
INNER
}

FAIL=0
say() { printf '%s\n' "$*"; }

# ---- ARM 1: the dropped box. The daemon must make the edge, and the box must have been
# deaf to whole pushes before it — both are read out of the emulator's own report.
A1=$(run_arm 1 2>&1) || true
case "$A1" in *"SKIP: "*) echo "${A1##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
say "$A1"
echo "$A1" | grep -q "PHY LINK-UP after a link-down" || { say "FAIL: the box never saw an edge — the daemon did not bounce its own port"; FAIL=1; }
echo "$A1" | grep -q "makes the edge itself" || { say "FAIL: the daemon never announced a wake edge"; FAIL=1; }
echo "$A1" | grep -q "BOXRC=0" || { say "FAIL: the dropped box did not enrol (fake_box exit != 0)"; FAIL=1; }
echo "$A1" | grep -q "ESTABLISHED" || { say "FAIL: the master never reached ESTABLISHED"; FAIL=1; }
DEAF=$(echo "$A1" | sed -n 's/.*DEAF to \([0-9]*\) complete.*/\1/p' | head -1)
[ -n "${DEAF:-}" ] && [ "$DEAF" -ge 1 ] || { say "FAIL: the box was deaf to no whole push — the master had not played the cheap rung before the edge"; FAIL=1; }

# ---- ARM 2: the positive control. Same everything, a box that never dropped: it enrols
# on the push alone and the daemon must NOT have bounced anything.
A2=$(run_arm 0 2>&1) || true
case "$A2" in *"SKIP: "*) say "SKIP: the control arm could not run"; exit $SKIP;; esac
say "$A2"
echo "$A2" | grep -q "BOXRC=0" || { say "FAIL (control): a box that answers pushes did not enrol at all"; FAIL=1; }
echo "$A2" | grep -q "makes the edge itself" && { say "FAIL (control): the daemon bounced a wire whose box was answering"; FAIL=1; }

[ "$FAIL" = 0 ] && say "PASS: a dropped box is re-acquired by the daemon alone (deaf to $DEAF whole pushes, then one PHY edge), and a box that answers is never bounced"
exit $FAIL
