#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: AN EMPTY PROBING MASTER GIVES ITS PORT'S LINK BUDGET TO THE SEGMENT THAT
# HAS A BOX ON IT -- and does NOT give it to one that only wants it (#107, auto-role spec
# amendment 2026-09-20).
#
# THE TRIGGER IS `S_SEGMENT_HEARD`, NOT A RECOGNISED BOX, and this run is where that was
# measured. A cold stagebox's presence flood is broadcast FILLER, classified role-UNKNOWN by
# construction; the config-announce that names it `S-1608` only arrives once a master is
# driving the wire. The arm below shows both: lb0.13 reads `unknown ... (16 ch)` while the
# budget is held, and `box ... S-1608 (16 in / 8 out)` seconds after it is released.
#
# THE NIGHT THIS IS ABOUT. 2026-09-20, the home rig: `enp131s0` links at 100 Mbit/s and
# carries the show rig's VLANs; the S-1608 sits on the untagged parent. Every segment
# starts `auto` and takes the wire as MASTER after 500 ms of silence, and `enp131s0.11` --
# an EMPTY VLAN, nothing on it, ever -- won that race. One 96 kHz master commits 97 024 of
# the port's 100 000 kbit/s, so the link-budget admission then refused the only segment
# that had a box on it, 690 times in 30 minutes, and the box never enrolled.
#
# WHY IT NEEDS A WHOLE BINARY AND A REAL WIRE. test_reac_link_budget proves the arithmetic
# and test_reac_hunt the verdict; neither can see the thing that was wrong, which is the
# ORDER two segments on one physical port reach the admission in and what the loser is
# allowed to do about it. That lives in main.c's listener table, the ifscan state machine
# and the sniffer's discovery table at once.
#
# THE PORT'S RATE IS DECLARED, NOT FAKED. A veth reports 10 Gbit/s through a private sysfs
# and nothing at all without one, so no arrangement of fake segments could ever fill a test
# port. `--set REACPW_LINK_MBIT=100` is the shipped knob for exactly this (docs/ENV-KNOBS.md,
# announced at start like every other override), and the run asserts the announce line --
# a knob that did not take is a test measuring a 10 Gbit port and calling it a rig.
#
# THE BOX IS THIS DAEMON'S OWN `role = box` SIDE, because the peer has to DECLARE itself
# before anybody masters it: that config-announce is what libreac classifies as a BOX
# sighting (reac_disco.c's REAC_CTRL_CONFIG_ANNOUNCE arm), and hearing a box is the whole
# trigger under test. libreac's fake_box is silent until a master pushes a complete scene
# transfer, which on a port whose budget is already held is a master that never starts.
#
# THE UNTAGGED PARENT IS IGNORED, by the shipped override and on purpose: a trunk parent
# with no tag heard yet would take the wire on silence like any other segment and hold the
# very budget this test is about. The rig's own remedy was the same file.
#
# ISOLATION IS PART OF THE TEST. An unprivileged user+net+mount+pid namespace, its own
# sysfs, its own PipeWire on a private runtime dir, the peer ends in a NESTED network
# namespace -- `unshare -n` isolates the wire and not the graph, and a lane daemon has
# appeared on the operator's live graph once already. Nothing here touches the live rig.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw}"
# ABSOLUTE, ALWAYS: nsenter into a mount namespace starts at /, so a relative path runs
# one side of the wire and silently fails to start the other.
BIN=$(readlink -f "$BIN")
SKIP=77

for t in unshare nsenter ip pipewire pw-cli python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

# ARM is "yield" (the empty holder must give the port up) or "held" (the holder has a box
# of its own and must keep it). One body, two arms, so the negative arm cannot drift into
# testing a different daemon from the positive one.
run_arm() {   # run_arm <yield|held>
	unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$1" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; ARM="$2"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# Wait up to N seconds for a pattern in the daemon's journal. Returns 1 and prints nothing
# when it never appears, so a slow machine costs time and not a false red.
wait_for() {   # wait_for <pattern> <secs>
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -qaE "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

# ---- ONE PHYSICAL PORT, TWO VLANS ON IT. That is the whole geometry of #107: the budget
# is the PARENT's, and `link_port_of` is what makes both children answer to one port.
ip link add lb0 type veth peer name lbp0 || exit 90
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 600' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link set lbp0 netns $NSPID || exit 90
ip link set lb0 up; $in_peer ip link set lbp0 up

for v in 11 13; do
	ip link add link lb0 name lb0.$v type vlan id $v || exit 90
	$in_peer ip link add link lbp0 name lbp0.$v type vlan id $v || exit 90
	$in_peer ip link set lbp0.$v up || exit 90
	ip link set lb0.$v down
done

# The untagged parent is not part of this question; see the header.
mkdir -p "$CONF/.config/reac-pw"
printf '[segment lb0]\nignore = yes\n' > "$CONF/.config/reac-pw/reac-pw.conf"

# ---- THE DAEMON, ON A PORT DECLARED 100 Mbit/s. One 96 kHz master costs 97 024 kbit/s of
# it, so the port carries exactly one -- which is the rig's own arithmetic, not a test one.
HOME="$CONF" REAC_DEBUG=1 "$BIN" --set REACPW_LINK_MBIT=100 >"$LOG" 2>&1 &
PID=$!
sleep 2
kill -0 $PID 2>/dev/null || { echo "daemon-died"; tail -8 "$LOG"; exit 91; }
grep -qa "S_KNOB_SET knob REACPW_LINK_MBIT=100" "$LOG" \
	&& echo "knob-announced yes" || echo "knob-announced no"

# ---- 1. THE EMPTY VLAN TAKES THE PORT. Nothing is on .11 and nothing ever will be; it
#         wins the wire on the masterless licence, exactly as enp131s0.11 did.
# THE BOX SIDE IS THIS DAEMON PINNED `role = box` IN THE HEARING SHAPE, never `--live`:
# `--live` takes its role from the command line, and a segment pinned in the file is what
# the box spec's own test drives (tests/box-declares-its-row.sh). Its conf IGNORES every
# interface it is not the box for, so one peer daemon cannot become a second master on a
# wire this test is measuring.
box_conf() {   # box_conf <home> <iface-that-is-a-box> [second]
	local home="$1"; shift
	mkdir -p "$home/.config/reac-pw"
	{
		printf '[segment lbp0]\nignore = yes\n'
		for v in 11 13; do
			case " $* " in
			*" lbp0.$v "*) printf '[segment lbp0.%s]\nrole = box\nmodel = s1608\n' "$v" ;;
			*)             printf '[segment lbp0.%s]\nignore = yes\n' "$v" ;;
			esac
		done
	} > "$home/.config/reac-pw/reac-pw.conf"
}

if [ "$ARM" = "held" ]; then
	# THE NEGATIVE ARM GIVES THE HOLDER ITS OWN BOX. Same binary, same wire, same
	# order -- the only difference is whether the holder is carrying anything.
	box_conf "$RT/boxes" lbp0.11 lbp0.13
	$in_peer env HOME="$RT/boxes" "$BIN" >"$RT/boxes.log" 2>&1 &
	sleep 3
fi
ip link set lb0.11 up
wait_for "S_SEGMENT_UP.*\[lb0\.11\]" 25 \
	&& echo "holder-up yes" || echo "holder-up no"
grep -qa "\[lb0\.11\] no REAC heard in" "$LOG" \
	&& echo "holder-on-silence yes" || echo "holder-on-silence no"

# ---- 2. THE SECOND VLAN COMES UP EMPTY AND IS REFUSED, and the refusal NAMES the holder.
#         690 lines of this in 30 minutes named the port and the number and left the
#         operator to dump the roster to find out who had it.
ip link set lb0.13 up
wait_for "E_LINK_BUDGET.*\[lb0\.13\]" 30 \
	&& echo "first-refusal yes" || echo "first-refusal no"
grep -a -A 8 "E_LINK_BUDGET.*\[lb0\.13\]" "$LOG" | grep -a "Held by:" | head -1 \
	| sed 's/^ */  held-by /'

# ---- 3. A BOX SPEAKS ON THE OTHER VLAN. It DECLARES itself -- which is what a cold
#         stagebox does on PHY-up and what the S-1608 of #107 did -- so the segment that
#         hears it is the one with something to carry.
if [ "$ARM" = "yield" ]; then
	box_conf "$RT/boxes" lbp0.13
	$in_peer env HOME="$RT/boxes" "$BIN" >"$RT/boxes.log" 2>&1 &
	BOXPID=$!
	sleep 2
	kill -0 $BOXPID 2>/dev/null && echo "box-running yes" || echo "box-running no"
else
	echo "box-running yes"
fi

wait_for "S_SEGMENT_HEARD.*\[lb0\.13\]" 40 \
	&& echo "box-heard yes" || echo "box-heard no"

# ---- 4. THE YIELD (or, in the `held` arm, its ABSENCE).
wait_for "S_BUDGET_YIELDED.*\[lb0\.11\]" 30 \
	&& echo "yielded yes" || echo "yielded no"
wait_for "S_SEGMENT_DROPPED.*\[lb0\.11\]" 15 \
	&& echo "holder-dropped yes" || echo "holder-dropped no"
wait_for "S_SEGMENT_UP.*\[lb0\.13\]" 40 \
	&& echo "taker-up yes" || echo "taker-up no"

# ---- 5. AND IT DOES NOT PING-PONG. A segment that yielded stays LISTENING until it hears
#         something ITSELF: the silence licence that won it the wire is just as true five
#         seconds later, and two empty segments would otherwise trade the port for ever.
sleep 12
echo "holder-ups $(grep -ca "S_SEGMENT_UP.*\[lb0\.11\]" "$LOG")"
echo "taker-drops $(grep -ca "S_SEGMENT_DROPPED.*\[lb0\.13\]" "$LOG")"
grep -qa "\[lb0\.11\] still LISTENING — this segment yielded" "$LOG" \
	&& echo "holder-listening yes" || echo "holder-listening no"

echo "--- refusals ---"
grep -a "E_LINK_BUDGET\|S_BUDGET_YIELDED\|S_SEGMENT_UP\|S_SEGMENT_DROPPED\|S_SEGMENT_HEARD" \
	"$LOG" | sed 's/^/  /' | head -40
kill -TERM $PID 2>/dev/null; sleep 0.5; kill -9 $PID 2>/dev/null
exit 0
INNER
}

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
val() { echo "$1" | grep -a "^$2 " | head -1 | awk '{print $2}'; }

# ---- ARM 1: THE YIELD --------------------------------------------------------------
echo "=== arm: yield (the holder is empty) ==="
OUT=$(run_arm yield); rc=$?
echo "$OUT" | sed 's/^/  /'
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || { echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT" | grep -qa '^SKIP:' && { echo "$OUT" | grep -a '^SKIP:'; exit $SKIP; }

# THE CONTROLS FIRST, every one of them. Each assertion below is about something that must
# or must not have happened LATER; a run in which the port was never capped, the holder
# never took it, or the box never spoke proves nothing in either direction, and would look
# exactly like a daemon that behaved.
[ "$(val "$OUT" knob-announced)" = "yes" ] \
	|| fail "REACPW_LINK_MBIT=100 was never announced — the port was not capped, so nothing below is a budget measurement"
[ "$(val "$OUT" holder-up)" = "yes" ] \
	|| fail "the empty VLAN never took the port — there was no holder to yield, and the rest of this arm is vacuous"
[ "$(val "$OUT" holder-on-silence)" = "yes" ] \
	|| fail "the holder did not take the wire on the masterless licence — this is not the race #107 is about"
[ "$(val "$OUT" first-refusal)" = "yes" ] \
	|| fail "the second VLAN was never refused on the budget — the admission is not in this path, so a later 'it came up' proves nothing about a yield"
echo "$OUT" | grep -qa "held-by .*lb0\.11 (probing, no box)" \
	|| fail "the refusal did not name lb0.11 as an empty prober (issue #107's whole ask about the message)"
[ "$(val "$OUT" box-running)" = "yes" ] \
	|| fail "the box side did not start"
[ "$(val "$OUT" box-heard)" = "yes" ] \
	|| fail "the box on lb0.13 was never HEARD — the yield's trigger never fired, so its absence below would mean nothing"
echo "$OUT" | grep -qa "S_SEGMENT_HEARD.*\[lb0\.13\].*($FACT_BOX_S1608_IN ch)" \
	|| fail "what lb0.13 heard was not the 16-channel box this arm started — something else is on that wire"

# ---- and now the claim itself
[ "$(val "$OUT" yielded)" = "yes" ] \
	|| fail "the empty holder did NOT yield the port to the segment that heard a box (#107)"
[ "$(val "$OUT" holder-dropped)" = "yes" ] \
	|| fail "S_BUDGET_YIELDED without S_SEGMENT_DROPPED: the sentence was printed and the engine was not taken down"
[ "$(val "$OUT" taker-up)" = "yes" ] \
	|| fail "the budget was released and the segment with the box still did not come up"
[ "$(val "$OUT" holder-ups)" = "1" ] \
	|| fail "the yielded segment took the wire again (S_SEGMENT_UP x$(val "$OUT" holder-ups)) — it must stay LISTENING until it hears something itself"
[ "$(val "$OUT" taker-drops)" = "0" ] \
	|| fail "the segment with the box was dropped again ($(val "$OUT" taker-drops)x) — the budget is ping-ponging"
[ "$(val "$OUT" holder-listening)" = "yes" ] \
	|| fail "the yielded segment never said it was still listening — a segment that goes quiet for no stated reason is what this project's journal rules forbid"

# ---- ARM 2: THE HOLDER HAS A BOX AND KEEPS THE PORT --------------------------------
echo "=== arm: held (the holder has a box of its own) ==="
OUT2=$(run_arm held); rc=$?
echo "$OUT2" | sed 's/^/  /'
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT2" | grep -qa 'daemon-died' && { echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && exit $SKIP
[ $rc -eq 0 ] || { echo "FAIL: the namespace body exited rc=$rc"; exit 1; }
echo "$OUT2" | grep -qa '^SKIP:' && { echo "$OUT2" | grep -a '^SKIP:'; exit $SKIP; }

[ "$(val "$OUT2" knob-announced)" = "yes" ] \
	|| fail "held arm: the port was never capped"
[ "$(val "$OUT2" holder-up)" = "yes" ] \
	|| fail "held arm: the holder never took the port"
[ "$(val "$OUT2" box-heard)" = "yes" ] \
	|| fail "held arm: the second VLAN's box was never heard, so its refusal below is not a decision about a box"
[ "$(val "$OUT2" first-refusal)" = "yes" ] \
	|| fail "held arm: the second VLAN was never refused on the budget"
[ "$(val "$OUT2" yielded)" = "no" ] \
	|| fail "held arm: a holder with a box of its own YIELDED — only an empty prober may (auto-role amendment 2026-09-20)"
[ "$(val "$OUT2" holder-dropped)" = "no" ] \
	|| fail "held arm: the holder's segment was dropped — a segment carrying a box is never taken down for a neighbour"
echo "$OUT2" | grep -qa "held-by .*lb0\.11 (probing, no box)" \
	&& fail "held arm: the refusal called a holder with a box an empty prober"
echo "$OUT2" | grep -qa "held-by .*lb0\.11 (" \
	|| fail "held arm: the refusal did not name the holder at all"

[ $FAIL -eq 0 ] || exit 1
echo "PASS: an empty probing master yields its port's link budget to the segment with REAC gear on it, and a holder with a box of its own does not"
