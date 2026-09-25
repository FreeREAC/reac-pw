#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# REAL SOCKETS: a segment in TAP role puts ZERO frames on the wire, measured from the
# other end.
#
# The ruling (openmixer master-arbitration, eighth amendment, 2026-09-13; the courtship
# trials of 2026-09-12): beside a real Roland desk, a courting slave of ours kept that
# desk's own S-1608 from enrolling for 180 s, and a granted one blocked it outright. So
# the console gains a role that never transmits — no announce, no join, no grant, no
# seglock, no TX socket — and this is where that "never" is measured rather than read.
#
# SILENCE IS AN ABSENCE, AND AN ABSENCE NEEDS A CONTROL. The ear here is
# courtship-probe's master end, which timestamps every frame arriving from ONE source
# MAC. Both arms below use the SAME ear with the SAME filter — the veth's own address —
# and differ in one thing only: which engine runs on our end.
#
#   ARM 1 (control): the real SLAVE engine, stamped with that MAC. The ear must hear
#                    thousands. Without this, arm 2's zero is a deaf probe.
#   ARM 2 (the claim): the real TAP engine. The ear must hear NOTHING — and tap-probe
#                    must ALSO report a stream it found, a rate it measured and frames
#                    it served, because a program that died at open is silent too.
#
# The peer end lives in a NESTED network namespace (tests/hearing-finds-a-segment.sh's
# header has the reasoning). Nothing leaves the namespace, so it cannot perturb a live
# REAC segment. Skips (77) where the namespaces are unavailable or the master could not
# hold its pacing — a measurement that could not be taken is not a pass.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
CP="${1:?usage: $0 /path/to/courtship-probe /path/to/tap-probe}"
TP="${2:?usage: $0 /path/to/courtship-probe /path/to/tap-probe}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v ip      >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

FPS=$FACT_PKT_RATE_48K   # 48 kHz: SAMPLE_RATE_48K / SAMPLES_PER_PKT
RUN=10

OUT=$(unshare -r -n --map-root-user bash -s -- "$CP" "$TP" "$FPS" "$RUN" <<'INNER'
set -u
CP="$1"; TP="$2"; FPS="$3"; RUN="$4"
RT=$(mktemp -d)
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$RT"; }
trap cleanup EXIT

unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }

ip link add tps0 type veth peer name tpm0 || exit 90
ip link set tpm0 netns $NSPID || exit 90
ip link set tps0 up
nsenter -t $NSPID -n ip link set tpm0 up

# OUR END'S OWN ADDRESS. Both arms are filtered on it, so the control and the claim are
# read through one ear: a slave stamped with it must be heard, and the tap — which stamps
# nothing because it sends nothing — must not be.
# Asked of NETLINK, not of /sys: this shell is in a new NETWORK namespace but /sys is
# still the host's mount, so /sys/class/net lists interfaces that are not ours and does
# not list the ones that are.
OURMAC=$(ip -o link show tps0 | tr ' ' '\n' | grep -A1 -x 'link/ether' | tail -n 1)
[ -n "$OURMAC" ] || { echo "SKIP: could not read tps0's MAC"; exit 77; }
echo "our_mac $OURMAC"

run_arm() {   # $1 = arm name, $2... = the command to run on OUR end
	local name="$1"; shift
	nsenter -t $NSPID -n "$CP" master tpm0 "$OURMAC" "$FPS" "$RUN" \
		>"$RT/$name.master" 2>"$RT/$name.master.err" &
	local MPID=$!
	sleep 0.6
	"$@" >"$RT/$name.ours" 2>"$RT/$name.ours.err" &
	local OPID=$!
	wait $MPID; local mrc=$?
	kill -TERM $OPID 2>/dev/null; wait $OPID 2>/dev/null
	# our own probe dying is a FAIL, not a SKIP (audit 2026-09-24, H3)
	[ $mrc -eq 0 ] || { echo "FAIL: the $name master end exited rc=$mrc";
	                    tail -n 3 "$RT/$name.master.err"; exit 94; }
	sed "s/^/$name /" "$RT/$name.master"
	sed "s/^/${name}_ours /" "$RT/$name.ours"
}

run_arm control "$CP" slave tps0 "$OURMAC" "$FACT_SAMPLE_RATE_48K" $((RUN - 2))
run_arm tap     "$TP" tps0 $((RUN - 2))
exit 0
INNER
)
rc=$?
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT" | grep -q '^SKIP:' && { echo "$OUT" | grep '^SKIP:'; exit $SKIP; }

echo "$OUT" | sed 's/^/  /'

# The census lines are prefixed with their arm, so a key is TWO fields and the value is
# the third — `control heard 21460`.
val() { echo "$OUT" | awk -v a="$1" -v k="$2" '$1 == a && $2 == k { print $3; f = 1; exit } END { exit !f }'; }
fail() { echo "FAIL: $1"; exit 1; }

CTRL_HEARD=$(val control heard)       || fail "the control arm printed no census"
CTRL_FPS=$(val control achieved_fps)  || fail "no achieved_fps in the control census"
TAP_HEARD=$(val tap heard)            || fail "the tap arm printed no census"
TAP_FPS=$(val tap achieved_fps)       || fail "no achieved_fps in the tap census"
STREAMS=$(val tap_ours streams)       || fail "tap-probe printed no stream count"
RATE=$(val tap_ours rate)             || fail "tap-probe printed no rate"
SERVED=$(val tap_ours served_frames)  || fail "tap-probe printed no served-frame count"

# 0. THE MEASUREMENT WAS TAKEN on both arms: an ear whose own pacing collapsed is
#    measuring the scheduler.
for f in "$CTRL_FPS" "$TAP_FPS"; do
	awk -v a="$f" -v t="$FPS" 'BEGIN { exit !(a > t * 0.7 && a < t * 1.3) }' || {
		echo "SKIP: a master paced $f fps against a target of $FPS"; exit $SKIP; }
done

# 1. THE CONTROL. The ear hears our end when our end transmits. Without this every
#    silence below is a deaf probe rather than a quiet role.
[ "$CTRL_HEARD" -gt 500 ] \
	|| fail "the ear heard only $CTRL_HEARD frames from a real SLAVE on the same MAC — the probe is deaf, so it cannot report a silence"

# 2. THE TAP DID ITS JOB. Silence from a program that died at open proves nothing.
[ "$STREAMS" -ge 1 ] || fail "the tap found $STREAMS streams — it heard nothing, so its silence says nothing"
[ "$RATE" -eq "$FACT_SAMPLE_RATE_48K" ] || fail "the tap measured $RATE Hz off a ${FPS} pps master; it should read $FACT_SAMPLE_RATE_48K"
[ "$SERVED" -gt 500 ] || fail "the tap served only $SERVED frames — it was not running"

# 3. AND IT PUT NOTHING ON THE WIRE. The whole claim, in one number.
[ "$TAP_HEARD" -eq 0 ] \
	|| fail "the tap role TRANSMITTED: the ear heard $TAP_HEARD frames from us. A tap announces nothing, joins nothing and is granted nothing"

echo "MEASURED: control heard $CTRL_HEARD frames from our MAC; the TAP was heard $TAP_HEARD times" \
     "while serving $SERVED frames over $STREAMS stream(s) at ${RATE} Hz"
echo "OK: a segment in tap role serves the wire and transmits zero frames on it"
