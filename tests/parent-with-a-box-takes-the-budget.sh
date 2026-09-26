#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: THE UNTAGGED PARENT THAT HEARS A BOX TAKES THE PORT FROM AN EMPTY VLAN THAT
# WON IT FIRST (#107, the rig's own geometry).
#
# THE ORDERING THE RIG COULD NOT FORCE. 2026-09-20 20:10: the S-1608 sits on the UNTAGGED
# parent `enp131s0`, the empty `enp131s0.11` took master on the 500 ms silence rule, and
# every later attempt of the parent was refused on the link budget -- 690 times in 30
# minutes. The rig run of the fix (issue comment, 21:48) proved the refusal line and a
# cold enrolment, and said in so many words that the YIELD was not exercised: the parent
# happened to win the race, so nothing had to give way. tests/empty-master-yields-the-
# budget.sh proves the yield VLAN-to-VLAN, with the parent IGNORED; nothing proved it for
# the taker the rig actually has, which is the parent. This test is that ordering, forced:
# the empty `.11` is brought up first and HOLDS the port before the parent has carrier.
#
# NO 8021q NEEDED, AND THAT IS NOT A SHORTCUT. The admission and the yield group segments
# into a physical port BY NAME -- `link_port_of` is `reac_declared_vlan_split`, which
# strips a numeric `.N` suffix and asks nothing of the kernel -- so a veth called `lb0.11`
# is on port `lb0` to every line of code under test here. A separate veth is used so this
# runs on a kernel without VLAN support (the cloud containers this project's CI runs on
# have none, and empty-master-yields-the-budget.sh SKIPs there). What it cannot see is a
# real trunk's shared wire; the rig acceptance in #107 stays the operator's.
#
# THE PORT'S RATE IS DECLARED (`--set REACPW_LINK_MBIT=100`), for the reason the sibling
# test gives: a veth reports 10 Gbit/s or nothing, and the run asserts the announce line.
#
# THE BOX IS THIS DAEMON'S OWN `role = box` SIDE on the parent's peer, as in the sibling:
# it DECLARES itself before anybody masters it, which is what makes the parent's hunt
# hear REAC gear -- the yield's only trigger.
#
# ISOLATION: an unprivileged user+net+mount+pid namespace with its own sysfs and its own
# PipeWire on a private runtime dir; the peer ends live in a NESTED network namespace.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
# ABSOLUTE, ALWAYS: nsenter into a mount namespace starts at /.
BIN=$(readlink -f "$BIN")
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# Wait up to N seconds for a pattern in the daemon's journal; a slow machine costs time,
# never a false red.
wait_for() {   # wait_for <pattern> <secs>
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -qaE "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

# ---- ONE PORT BY NAME: the parent `lb0` (the box's wire) and `lb0.11` (empty, ever).
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 600' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
for i in lb0 lb0.11; do
	ip link add $i type veth peer name ${i/lb0/lbp0} || exit 90
	ip link set ${i/lb0/lbp0} netns $NSPID || exit 90
	$in_peer ip link set ${i/lb0/lbp0} up || exit 90
	ip link set $i down
done

# ---- THE BOX, on the parent's peer. Its conf ignores the empty wire's peer, so one peer
#      daemon can never become a second master on the wire this test measures.
mkdir -p "$RT/box/.config/reac-pw"
printf '[segment lbp0]\nrole = box\nmodel = s1608\n[segment lbp0.11]\nignore = yes\n' \
	> "$RT/box/.config/reac-pw/reac-pw.conf"
$in_peer env HOME="$RT/box" "$BIN" >"$RT/box.log" 2>&1 &
BOXPID=$!

# ---- THE DAEMON, NO CONFIGURATION AT ALL -- the operator's acceptance is "nothing written
#      anywhere" -- on a port declared 100 Mbit/s, which carries exactly one 96 kHz master.
mkdir -p "$CONF/.config"
HOME="$CONF" REAC_DEBUG=1 "$BIN" --set REACPW_LINK_MBIT=100 >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }
kill -0 $BOXPID 2>/dev/null && echo "box-running yes" || echo "box-running no"
grep -qa "S_KNOB_SET knob REACPW_LINK_MBIT=100" "$LOG" \
	&& echo "knob-announced yes" || echo "knob-announced no"

# ---- 1. THE EMPTY VLAN WINS THE PORT, before the parent has carrier: the 20:10 ordering.
ip link set lb0.11 up
wait_for "S_SEGMENT_UP.*\[lb0\.11\]" 25 \
	&& echo "holder-up yes" || echo "holder-up no"
grep -qa "\[lb0\.11\] no REAC heard in" "$LOG" \
	&& echo "holder-on-silence yes" || echo "holder-on-silence no"

# ---- 2. THE PARENT, WITH THE BOX ON IT, COMES UP SECOND.
ip link set lb0 up
wait_for "S_SEGMENT_HEARD.*\[lb0\]" 40 \
	&& echo "box-heard yes" || echo "box-heard no"
wait_for "S_BUDGET_YIELDED.*\[lb0\.11\]" 30 \
	&& echo "yielded yes" || echo "yielded no"
wait_for "S_SEGMENT_UP.*\[lb0\]" 30 \
	&& echo "taker-up yes" || echo "taker-up no"
# THE JOB, NOT A PART OF IT: the box is enrolled on the parent, by model.
wait_for "S_SEGMENT_HEARD.*\[lb0\].*S-1608" 40 \
	&& echo "box-enrolled yes" || echo "box-enrolled no"

# ---- 3. AND IT STAYS THAT WAY: the yielded VLAN does not take the port back on silence.
sleep 12
echo "holder-ups $(grep -ca "S_SEGMENT_UP.*\[lb0\.11\]" "$LOG")"
echo "taker-drops $(grep -ca "S_SEGMENT_DROPPED.*\[lb0\]" "$LOG")"

echo "--- journal ---"
grep -a "E_LINK_BUDGET\|Held by\|S_BUDGET_YIELDED\|S_SEGMENT_UP\|S_SEGMENT_DROPPED\|S_SEGMENT_HEARD" \
	"$LOG" | sed 's/^/  /' | head -40
exit 0
INNER
)
rc=$?
echo "$OUT" | sed 's/^/  /'
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs, 77 is the only
# SKIP, any other rc FAILs.
echo "$OUT" | grep -qa 'daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || { echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT" | grep -qa '^SKIP:' && { echo "$OUT" | grep -a '^SKIP:'; exit $SKIP; }

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
val() { echo "$OUT" | grep -a "^$1 " | head -1 | awk '{print $2}'; }

# THE CONTROLS FIRST: a run in which the port was never capped, the empty VLAN never held
# it, or the box never spoke proves nothing either way.
[ "$(val knob-announced)" = "yes" ] \
	|| fail "REACPW_LINK_MBIT=100 was never announced — the port was not capped"
[ "$(val box-running)" = "yes" ] || fail "the box side did not start"
[ "$(val holder-up)" = "yes" ] \
	|| fail "the empty lb0.11 never took the port — there was no holder, so this is not the 20:10 ordering"
[ "$(val holder-on-silence)" = "yes" ] \
	|| fail "lb0.11 did not take the wire on the silence licence — not the race #107 is about"
[ "$(val box-heard)" = "yes" ] \
	|| fail "the box on the parent was never HEARD — the yield's trigger never fired"

# ---- the claim
[ "$(val yielded)" = "yes" ] \
	|| fail "the empty lb0.11 did NOT yield the port to the parent that heard a box (#107)"
[ "$(val taker-up)" = "yes" ] \
	|| fail "the parent with the box on it never came up — the 690-refusals night"
[ "$(val box-enrolled)" = "yes" ] \
	|| fail "the parent came up and the S-1608 was never recognised on it"
[ "$(val holder-ups)" = "1" ] \
	|| fail "the yielded lb0.11 took the port again (S_SEGMENT_UP x$(val holder-ups)) on silence"
[ "$(val taker-drops)" = "0" ] \
	|| fail "the parent with the box was dropped again ($(val taker-drops)x) — the budget ping-pongs"

[ $FAIL -eq 0 ] || exit 1
echo "PASS: an empty VLAN that won the port first yields it to the untagged parent that hears a box, and the box enrols there"
