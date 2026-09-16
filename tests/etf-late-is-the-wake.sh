#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: under the ETF backend the health line's `late` figure is the pacer
# THREAD'S WAKE, and the wire is exact anyway.
#
# THE FAULT THIS EXISTS FOR. Since ETF became the default (1.0.7) the desk's
# `reac-health:` line has read 19-41 late/s at any graph quantum, with no
# instrumentation running — while pace_hist on the TX device measured the release
# instant at 1.1 us of stddev and late>=1.5x at 0.08/s
# (reac-captures/pace-compare-2026-09-14/daemon-owned-1.0.7/etf/wire.txt). Both
# numbers were right. They are not the same measurement: the thread sleeps to
# `launch - lead` (2500 us by default), stamps the frame with an absolute launch
# time, and the kernel's etf qdisc releases it at that instant. Everything the thread
# does inside the lead is invisible to the wire.
#
# WHAT A GREEN SUITE COULD NOT SEE. Every unit test around the pacer asserts the
# counter it has; none of them can say whether that counter observes the thing the
# operator is being asked to read. So this measures three things at once, per arm:
#
#   the daemon's own health line   the figure on trial
#   the etf qdisc's drop counter   what the KERNEL says it refused to launch,
#                                  read through the daemon's own door
#                                  (reac_qdisc_stats_read), never `tc -s`
#   the WIRE                       libreac's pace_hist over a tcpdump taken at the
#                                  FAR END of the veth — the accused does not get
#                                  to be the witness
#
# THE POSITIVE CONTROL IS THE ARM THAT MAKES IT A PROOF. `etfshort` runs the same
# daemon with the lead cut to 400 us, under the thread's own measured wake tail, so
# launch times really are missed. The qdisc's drops go up by an order of magnitude
# and the wire loosens; `late`/`wake-late` does not move. A probe that reports
# absence has to be shown detecting presence first.
#
# ISOLATION, both halves, exactly as tests/etf-qdisc-owned.sh: a user+net+pid
# namespace with its own veth AND its own PipeWire on a private runtime dir, because
# `unshare -n` isolates the wire and not the graph. Proven in both directions.
#
# Skips (77) without the namespaces, iproute2, sch_etf, tcpdump or PipeWire.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/etf_wire_probe}"
PROBE="${2:?usage: $0 /path/to/reac-pw /path/to/etf_wire_probe}"
SKIP=77
# Two health windows (10 s each) plus the daemon's start and the box's join.
SECS="${REACPW_LATE_SECS:-24}"

for t in unshare ip tcpdump pipewire pw-cli pw-dump; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
grep -q '^sch_etf ' /proc/modules 2>/dev/null || {
	echo "SKIP: sch_etf is not loaded and a user namespace may not autoload it"
	echo "      (\`sudo modprobe sch_etf\` on the host, then rerun)"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

# libreac's pace_hist is the WIRE instrument. It lives in the other repo, so it is
# handed in; without it the wire half is announced, never skipped in silence.
HIST="${REACPW_PACE_HIST:-}"
[ -n "$HIST" ] && [ -x "$HIST" ] || HIST=""

OUT=$(REACPW_PACE_HIST="$HIST" unshare -r -n -p -f --mount-proc --map-root-user \
      bash -s -- "$BIN" "$PROBE" "$SECS" <<'INNER'
set -u
BIN="$1"; PROBE="$2"; SECS="$3"; HIST="${REACPW_PACE_HIST:-}"
RT=$(mktemp -d)
cleanup() {
	kill -TERM $(jobs -p) 2>/dev/null
	sleep 0.3
	kill -9 $(jobs -p) 2>/dev/null
	rm -rf "$RT"
}
trap cleanup EXIT

# THE GRAPH IS ISOLATED HERE AND NOWHERE ELSE (etf-qdisc-owned.sh, same reason).
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || { echo "SKIP: no private PipeWire in the namespace"; exit 77; }
echo "private-graph-socket $RT"

ip link add etfa type veth peer name etfb || exit 90
ip link set etfa up
ip link set etfb up
DAEMON="--live etfa --tx etfa --mixer m5000 --rate 96000"

# One arm: capture the far end, run the daemon under $2's environment for $SECS,
# read the qdisc counters through the daemon's own door, then read the wire.
#   $1 label   $2 env assignments
arm() {
	local label="$1" envs="$2"
	# -Z root: tcpdump drops to the `tcpdump` user and then cannot own the savefile
	#   in this namespace, and writes an EMPTY pcap while printing nothing that looks
	#   like a failure — the absence-looks-like-silence shape, met here once.
	tcpdump -i etfb -w "$RT/$label.pcap" -s 128 -U -n -Z root >"$RT/$label.td" 2>&1 &
	local td=$!
	sleep 1
	# `exec`, so the background PID IS the daemon: a SIGTERM to a wrapping shell
	# leaves the daemon holding the segment lock and its qdisc for the next arm.
	eval "exec env $envs $BIN $DAEMON" >"$RT/$label.log" 2>&1 &
	local pid=$!
	sleep 1
	if ! kill -0 $pid 2>/dev/null; then
		echo "$label-daemon-died"
		sed 's/^/  '"$label"': /' "$RT/$label.log" | tail -5
		kill -TERM $td 2>/dev/null
		return
	fi
	sleep "$SECS"
	echo "$label-$("$PROBE" stats etfa)"
	# Kept as INFORMATION, not as the control: since 2026-09-16 a master with no box
	# recognized publishes NO NODE AT ALL, so this reads 0 on a healthy run.
	echo "$label-privatenodes $(pw-dump 2>/dev/null | grep -ac 'reac-playback\|reac-capture')"
	kill -TERM $pid 2>/dev/null
	for i in $(seq 40); do kill -0 $pid 2>/dev/null || break; sleep 0.2; done
	kill -9 $pid 2>/dev/null
	wait $pid 2>/dev/null
	sleep 1
	kill -TERM $td 2>/dev/null
	wait $td 2>/dev/null

	# The LAST closed window of the run, so the figure is not the startup transient.
	local h
	h=$(grep -a 'reac-health:' "$RT/$label.log" | tail -1)
	echo "$label-health ${h:-NONE}"
	echo "$label-budget $(grep -a 'slot-debt catch-up' "$RT/$label.log" | tail -1)"
	if [ -n "$HIST" ]; then
		# The daemon's own MAC is the talker at the REAC pace; the far end also
		# carries the emulated box's upstream, which pace_hist reports separately.
		"$HIST" --fps 8000 "$RT/$label.pcap" 2>&1 \
			| grep -aA3 'rate_hz=96000' | sed "s/^/$label-wire /"
	else
		echo "$label-wire NOT-RUN"
	fi
	tc qdisc del dev etfa root 2>/dev/null
}

# 1. THE DEFAULT. ETF, the shipped 2500 us lead.
arm etfdefault "-u REACPW_PACER"
# 2. THE POSITIVE CONTROL. The same daemon, lead cut under the wake tail: launch
#    times are really missed, and the instrument has to see it.
arm etfshort "-u REACPW_PACER REACPW_PACER_LEAD_US=400"
# 3. THE THREAD BACKEND, whose `late` means what it always meant.
arm thread "REACPW_PACER=thread"
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
line() { echo "$OUT" | grep -a "^$1" | head -1; }
# drops / packets out of "<arm>-stats qdiscs=N packets=N drops=N overlimits=N"
stat() { line "$1-stats" | sed -n "s/.*[ =]$2=\([0-9]*\).*/\1/p"; }
# a named float out of the health line, e.g. hnum etfdefault 'wake-late'
hnum() { echo "$OUT" | grep -a "^$1-health" | sed -n "s/.*$2 \([0-9.]*\)\/s.*/\1/p"; }
wire() { echo "$OUT" | grep -a "^$1-wire" | sed -n "s/.*$2=\([0-9.]*\).*/\1/p" | head -1; }
gt()   { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 > b+0)}'; }

for a in etfdefault etfshort thread; do
	echo "$OUT" | grep -aq "^$a-daemon-died" && fail "the $a arm's daemon died at start"
	echo "$OUT" | grep -aq "^$a-health reac-health:" \
		|| fail "the $a arm closed no health window in ${REACPW_LATE_SECS:-24} s — nothing below can be read"
done

# 0. THE GRAPH WAS ISOLATED, PROVEN IN BOTH DIRECTIONS (the positive first: a
#    negative alone passes for a daemon that published nothing anywhere).
# THE POSITIVE HALF IS THE HEALTH WINDOW, NOT A NODE. It used to count reac-* nodes on the
# private graph; a master with no box recognized publishes none since 2026-09-16 (spec
# 2026-09-16-segments-and-roles-are-autodetected.md, "no recognised box, no node"), so that
# control now reads 0 on a perfectly healthy daemon. The daemon's own health window —
# closed by ITS pacer, inside THIS namespace, on the netdev this namespace owns — is the
# positive that remains, and the arms below are built on it anyway.
echo "$OUT" | grep -aq '^etfdefault-health reac-health:' \
	|| fail "the daemon closed no health window at all — it did nothing HERE, so the negative below cannot mean it did nothing THERE either"
if pw-dump >/dev/null 2>&1; then
	pw-dump 2>/dev/null | grep -aq -- "$BIN" \
		&& fail "a process from $BIN is on the OPERATOR'S live PipeWire graph — this test leaked out of its namespace"
	echo "  isolation: the live graph carries nothing from $BIN"
fi

# 1. THE VOCABULARY. Under ETF the line names what it measures and carries the budget
#    the wake lateness is spent against; under thread it is untouched.
echo "$OUT" | grep -a '^etfdefault-health' | grep -q 'launch-miss' \
	|| fail "the ETF health line does not report launch misses: $(line etfdefault-health)"
echo "$OUT" | grep -a '^etfdefault-health' | grep -q 'wake-late .* of a 2500 us lead' \
	|| fail "the ETF health line does not report the wake lateness against its lead: $(line etfdefault-health)"
echo "$OUT" | grep -a '^thread-health' | grep -q 'late .*/s (catchup' \
	|| fail "the THREAD health line changed — its late/s is the egress instant and must keep its meaning: $(line thread-health)"
echo "$OUT" | grep -a '^thread-health' | grep -q 'launch-miss' \
	&& fail "the thread backend has no qdisc and no lead; it must not report launch misses"

# 2. THE ETF CATCH-UP BUDGET IS THE LEAD, not the thread backend's wake tail.
echo "$OUT" | grep -a '^etfdefault-budget' | grep -q '17 slots = 2125 us at 8000 fps' \
	|| fail "the ETF arm's catch-up budget is not the lead-derived 17 slots: $(line etfdefault-budget)"
echo "$OUT" | grep -a '^thread-budget' | grep -q '8 slots = 1000 us at 8000 fps' \
	|| fail "the thread arm's budget must stay libreac's measured 1000 us: $(line thread-budget)"

# 3. THE INSTRUMENT CAN SEE. A qdisc reading that found no etf qdisc would make every
#    drop figure below a broken search.
[ "$(stat etfdefault qdiscs)" -ge 1 ] 2>/dev/null \
	|| fail "no etf qdisc was found on etfa while the ETF arm ran: $(line etfdefault-stats)"
[ "$(stat etfdefault packets)" -gt 10000 ] 2>/dev/null \
	|| fail "the etf qdisc passed only $(stat etfdefault packets) packets — the wire is not carrying"

# 4. THE FINDING, STATED AS A RATIO because an absolute figure here is a measure of
#    how busy the host was. On the shipped lead the number an operator reads (`late`,
#    now `wake-late`) is at least five times what the wire actually lost — 135x on an
#    idle box, and the desk's own capture put it near 300x.
WL=$(hnum etfdefault 'wake-late'); LM=$(hnum etfdefault 'launch-miss')
[ -n "$WL" ] && [ -n "$LM" ] || fail "could not read the ETF figures: $(line etfdefault-health)"
gt "$WL" 1 || fail "the ETF arm reported only $WL wake-late/s — this run did not reproduce the desk's 19-41/s and proves nothing"
awk -v w="$WL" -v m="$LM" 'BEGIN{ exit !(w+0 > 5*(m+0)) }' \
	|| fail "the ETF arm's wake-late ($WL/s) was not 5x its launch misses ($LM/s) — on this run the old figure was not misleading and the lane's premise does not hold"

# 5. THE POSITIVE CONTROL. Cut the lead under the wake tail and launch times really
#    are missed: the KERNEL's counter moves and so does the wire. The wake figure does
#    not. A probe that reports absence has to be shown detecting presence first, and
#    this is that showing.
D0=$(stat etfdefault drops); P0=$(stat etfdefault packets)
D1=$(stat etfshort drops);   P1=$(stat etfshort packets)
[ -n "$D1" ] && [ "$P1" -gt 10000 ] 2>/dev/null \
	|| fail "the short-lead control did not carry: $(line etfshort-stats)"
awk -v d0="$D0" -v p0="$P0" -v d1="$D1" -v p1="$P1" '
	BEGIN { r0=(d0+0)/(p0+0); r1=(d1+0)/(p1+0);
	        printf "  control: drop rate %.2e (lead 2500) -> %.2e (lead 400)\n", r0, r1;
	        exit !(r1 > 2*r0 && r1 > 0) }' \
	|| fail "shrinking the lead to 400 us did not raise the qdisc's drop rate ($D0/$P0 -> $D1/$P1) — this instrument has not been shown to detect a launch miss at all"
# The same control from OUTSIDE the daemon, when the wire instrument is present: a
# missed launch is a frame that never left, and pace_hist counts the gap it leaves.
L1P5_0=$(wire etfdefault 'late1p5'); L1P5_1=$(wire etfshort 'late1p5')
if [ -n "$L1P5_0" ] && [ -n "$L1P5_1" ]; then
	awk -v a="$L1P5_0" -v b="$L1P5_1" 'BEGIN{ exit !(b+0 > 5*(a+0) && b+0 > 20) }' \
		|| fail "the short-lead arm's WIRE did not loosen (late>=1.5x: $L1P5_0 -> $L1P5_1) — the control did not actually break anything, so it controls nothing"
	echo "  control: wire late>=1.5x $L1P5_0 (lead 2500) -> $L1P5_1 (lead 400)"
fi
WL1=$(hnum etfshort 'wake-late')
awk -v a="$WL" -v b="$WL1" 'BEGIN{ exit !(b+0 > 5*(a+0)) }' \
	&& fail "the wake figure tracked the fault ($WL -> $WL1 /s); the claim that it is blind to the wire does not hold in this run"
echo "  the wake figure did not track the fault: $WL/s -> $WL1/s while the drops and the wire did"

# 6. THE WIRE, from the far end. ETF's release instant is tighter than the thread's
#    under the same load, while both report the same order of late wakes.
SD_E=$(wire etfdefault 'iv_sd_us'); SD_T=$(wire thread 'iv_sd_us')
if [ -n "$SD_E" ] && [ -n "$SD_T" ]; then
	gt "$SD_T" "$SD_E" \
		|| fail "the ETF arm's wire (sd ${SD_E} us) was not tighter than the thread arm's (sd ${SD_T} us) — the premise of this whole test"
	WLT=$(hnum thread 'late')
	echo "  wire sd: etf ${SD_E} us vs thread ${SD_T} us, on late/s $WL vs $WLT"
else
	echo "  wire: NOT RUN (hand libreac's pace_hist in REACPW_PACE_HIST for the wire half)"
fi

echo "OK"
exit 0
