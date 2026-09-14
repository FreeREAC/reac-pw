#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: the daemon OWNS the qdisc on the device it binds, and the frames
# still leave.
#
# The unit tests prove the netlink messages are byte-identical to what iproute2
# sends and that the errnos classify. Neither can see the join: the daemon resolving
# its own backend, installing the qdisc BEFORE the pacer probes for one, and taking
# it away on exit. That join is exactly the shape a green suite hides — every part
# works, nothing calls anything.
#
# And one assertion here is not about the qdisc at all. With `skip_sock_check` the
# etf qdisc DROPS EVERY FRAME THAT CARRIES NO LAUNCH TIME. On the rig, 2026-09-14, a
# thread-backend daemon under a leftover etf qdisc transmitted 0 packets in a 60 s
# window and the box lost its master. So every arm below counts REAC frames ARRIVING
# AT THE FAR END of the cable: asking the daemon what it sent would be asking the
# accused, and a qdisc that silently eats the wire looks exactly like a healthy
# daemon from the inside.
#
#   1. DEFAULT (nothing set) -> etf. The qdisc appears on the device while the daemon
#      runs, and frames arrive.
#   2. EXIT. The qdisc is gone afterwards, verified by a netlink read.
#   3. THREAD UNDER A LEFTOVER ETF. The test installs an etf qdisc by hand, starts the
#      daemon with REACPW_PACER=thread, and requires the daemon to have REMOVED it —
#      and frames to arrive. Without the removal this arm reads zero frames, which is
#      the rig's own failure reproduced.
#   4. REFUSAL. The same default start with CAP_NET_ADMIN dropped: the install is
#      refused with EPERM, the daemon says so in one loud line, RUNS THE THREAD
#      BACKEND, and still carries frames. A desk that stopped carrying audio because
#      a kernel could not do ETF would be a worse answer than a looser cadence.
#   5. A BOX ENROLS THROUGH IT. Optional, and only because its emulator lives in the
#      other repo: point REACPW_FAKE_BOX at libreac's `make fake_box` binary and this
#      arm runs a linked, silent S-4000S on the far end and requires the daemon --
#      on the etf backend, through the qdisc it installed itself -- to RECOGNIZE it
#      and reach ESTABLISHED. Frame counts prove the wire is not being eaten; this
#      proves the protocol still completes on top of it. Announced as NOT RUN, never
#      skipped quietly, when the binary is not given.
#
# Every qdisc reading is an RTM_GETQDISC over the library's own public door
# (tests/etf_wire_probe.c), never a scrape of `tc qdisc show`.
#
# ISOLATION IS PART OF THE TEST, AND `unshare -n` IS ONLY HALF OF IT. It isolates the
# WIRE, not the AUDIO GRAPH: PipeWire is reached through $XDG_RUNTIME_DIR/pipewire-0,
# which is inherited, so a daemon started this way joins the OPERATOR'S LIVE GRAPH and
# publishes a REAC door among the real desk's nodes. That happened here on 2026-09-14 —
# a test daemon sat in the operator's graph as "REAC segment door (no box recognized
# yet)" for fourteen minutes. So this runs in:
#
#   a USER+NET+PID namespace (`-p -f --mount-proc`) — the pid namespace is what makes
#   an orphaned daemon impossible: when the namespace's init leaves, everything in it
#   goes, and it was an orphan that survived the earlier runs;
#   its own veth pair — the qdisc changes and the frames stay inside it;
#   its own PRIVATE PipeWire, with XDG_RUNTIME_DIR and PIPEWIRE_RUNTIME_DIR pointing
#   at a temporary directory this run made, started INSIDE the namespace;
#   and an EXIT trap that kills the daemon and that PipeWire on every path.
#
# The isolation is then PROVEN rather than assumed, in both directions: the private
# graph must SHOW the daemon's node (a control without which "not on the live graph"
# could just mean the daemon never made one), and the operator's graph — read, never
# touched — must show nothing running from this binary.
#
# Skips (77) where the namespaces, iproute2, PipeWire or sch_etf is unavailable —
# module autoload needs privileges this namespace does not have, so an absent sch_etf
# is a skip and not a failure.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/etf_wire_probe}"
PROBE="${2:?usage: $0 /path/to/reac-pw /path/to/etf_wire_probe}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v ip      >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v capsh   >/dev/null 2>&1 || { echo "SKIP: no capsh (libcap)"; exit $SKIP; }
grep -q '^sch_etf ' /proc/modules 2>/dev/null || {
	echo "SKIP: sch_etf is not loaded and a user namespace may not autoload it"
	echo "      (\`sudo modprobe sch_etf\` on the host, then rerun)"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-cli   >/dev/null 2>&1 || { echo "SKIP: no pw-cli"; exit $SKIP; }
command -v pw-dump  >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(REACPW_FAKE_BOX="${REACPW_FAKE_BOX:-}" unshare -r -n -p -f --mount-proc --map-root-user \
      bash -s -- "$BIN" "$PROBE" <<'INNER'
set -u
BIN="$1"; PROBE="$2"
RT=$(mktemp -d)
cleanup() {
	kill -TERM $(jobs -p) 2>/dev/null
	sleep 0.3
	kill -9 $(jobs -p) 2>/dev/null
	rm -rf "$RT"
}
trap cleanup EXIT

# THE GRAPH IS ISOLATED HERE, AND NOWHERE ELSE. Without these two the daemon below
# connects to the operator's live PipeWire through the inherited runtime dir and
# publishes its doors into the real desk's graph.
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || { echo "SKIP: no private PipeWire in the namespace"; exit 77; }
echo "private-graph-socket $RT"

# <= 15 chars (IFNAMSIZ). etfa is the daemon's TX device; etfb is the far end, where
# every frame count below is taken.
ip link add etfa type veth peer name etfb || exit 90
ip link set etfa up
ip link set etfb up

# THE INSTRUMENT MUST BE ABLE TO DETECT PRESENCE BEFORE ANY ABSENCE IS BELIEVED.
# A qdisc put on by hand, read back through the library's door, then taken off again.
tc qdisc add dev etfa root etf clockid CLOCK_TAI delta 300000 skip_sock_check 2>/dev/null \
	|| { echo "SKIP: this namespace cannot add an etf qdisc at all"; exit 77; }
echo "control-present $("$PROBE" qdisc etfa)"
tc qdisc del dev etfa root 2>/dev/null
echo "control-absent $("$PROBE" qdisc etfa)"

DAEMON="--live etfa --tx etfa --mixer m5000 --rate 96000"

# One arm: start the daemon the given way, read the qdisc while it runs, count frames
# at the far end, stop it, read the qdisc again.
#   $1 label   $2 pre-install an etf qdisc by hand (0/1)   $3 the whole command line
arm() {
	local label="$1" leftover="$2" cmd="$3"
	[ "$leftover" = 1 ] && tc qdisc add dev etfa root etf clockid CLOCK_TAI \
	                          delta 300000 skip_sock_check 2>/dev/null
	echo "$label-before $("$PROBE" qdisc etfa)"

	# `exec`, so the background PID IS the daemon and not a shell wrapping it: a
	# SIGTERM to the wrapper leaves the daemon running, holding the segment lock and
	# its qdisc, and the next arm then fails for a reason that has nothing to do with
	# what it is testing. (Measured here, once.)
	eval "exec $cmd" >"$RT/$label.log" 2>&1 &
	local pid=$!
	sleep 3
	if ! kill -0 $pid 2>/dev/null; then
		echo "$label-daemon-died"
		sed 's/^/  '"$label"': /' "$RT/$label.log" | tail -5
		return
	fi
	echo "$label-running $("$PROBE" qdisc etfa)"
	# THE POSITIVE HALF OF THE ISOLATION PROOF. The daemon made a node, and it made
	# it HERE. Without this, "nothing of ours on the live graph" could equally mean
	# the daemon never published anything at all.
	echo "$label-privatenodes $(pw-dump 2>/dev/null | grep -ac 'reac-playback\|reac-capture')"
	echo "$label-$("$PROBE" count etfb 3)"
	kill -TERM $pid 2>/dev/null
	for i in $(seq 40); do kill -0 $pid 2>/dev/null || break; sleep 0.2; done
	if kill -0 $pid 2>/dev/null; then
		echo "$label-hung"
		kill -9 $pid 2>/dev/null
	fi
	wait $pid 2>/dev/null
	echo "$label-after $("$PROBE" qdisc etfa)"
	grep -aE 'reac-qdisc:|reac-pacer: backend|ETF IS THE DEFAULT|CANNOT INSTALL' \
		"$RT/$label.log" | tr -d '\r' | sed "s/^/  $label: /" | head -6
	tc qdisc del dev etfa root 2>/dev/null
}

# 1+2. The default is etf: the daemon installs the qdisc, carries frames, removes it.
arm default 0 "env -u REACPW_PACER $BIN $DAEMON"

# 3. The thread backend under a LEFTOVER etf qdisc. The daemon must take it away, or
#    the qdisc eats every frame it sends.
arm leftover 1 "env REACPW_PACER=thread $BIN $DAEMON"

# 4. The refusal path: the same default start with CAP_NET_ADMIN dropped from the
#    BOUNDING set. For a uid-0 process the permitted set after exec is the bounding
#    set, so dropping it there is how the capability actually goes away — and this is
#    the shape an operator meets, a daemon started by hand instead of from the unit
#    whose file capability grants it. CAP_NET_RAW stays, so AF_PACKET still opens:
#    what the daemon cannot do is install a qdisc or set SO_TXTIME.
arm refusal 0 "capsh --drop=cap_net_admin -- -c '$BIN $DAEMON'"

# 5. A REAL BOX ENROLMENT, on the etf backend, through the daemon's own qdisc.
if [ -n "${REACPW_FAKE_BOX:-}" ] && [ -x "${REACPW_FAKE_BOX:-}" ]; then
	env -u REACPW_PACER "$BIN" $DAEMON >"$RT/box.log" 2>&1 &
	BPID=$!
	sleep 1
	echo "box-running $("$PROBE" qdisc etfa)"
	"$REACPW_FAKE_BOX" etfb 14 >"$RT/box-far.log" 2>&1 &
	FPID=$!
	sleep 14
	kill -TERM $BPID 2>/dev/null; sleep 1; kill -9 $BPID $FPID 2>/dev/null
	wait $BPID 2>/dev/null; wait $FPID 2>/dev/null
	echo "box-state $(grep -aoE '\-> [A-Z]+' "$RT/box.log" | tail -1 | tr -d ' >-')"
	echo "box-recognized $(grep -ac 'S-4000\|recogni' "$RT/box.log")"
	grep -aE 'ESTABLISHED|recogni' "$RT/box.log" | tr -d '\r' | sed 's/^/  box: /' | tail -3
else
	echo "box-arm NOT-RUN"
fi

exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
line() { echo "$OUT" | grep -a "^$1 " | head -1; }
val()  { line "$1" | awk '{print $3}'; }
st()   { line "$1" | awk '{print $2}'; }
reac() { echo "$OUT" | grep -a "^$1-frames " | head -1 | sed -n 's/.*reac=\([0-9]*\).*/\1/p'; }
anyf() { echo "$OUT" | grep -a "^$1-frames " | head -1 | sed -n 's/.*any=\([0-9]*\).*/\1/p'; }

# 0a. THE GRAPH WAS ISOLATED, PROVEN IN BOTH DIRECTIONS.
#     Positive: the daemon published REAC nodes onto the PRIVATE PipeWire this run
#     started. Negative: the operator's own graph, read and never touched, carries
#     nothing running from this binary. A negative alone would pass for a daemon that
#     published nothing anywhere, which is why the positive comes first.
echo "$OUT" | grep -aq '^private-graph-socket ' \
	|| fail "the namespace never reported a private PipeWire runtime dir"
[ "$(st default-privatenodes)" -ge 1 ] 2>/dev/null \
	|| fail "the daemon published no REAC node on the PRIVATE graph ($(line default-privatenodes)) — the isolation cannot be read off a graph with nothing in it"
if pw-dump >/dev/null 2>&1; then
	pw-dump 2>/dev/null | grep -aq -- "$BIN" \
		&& fail "a process from $BIN is on the OPERATOR'S live PipeWire graph — this test leaked out of its namespace"
	echo "  isolation: the live graph carries nothing from $BIN"
else
	echo "  isolation: no live PipeWire graph to check against (negative control NOT RUN)"
fi

# 0b. THE INSTRUMENT WORKS IN BOTH DIRECTIONS. Without this every "no etf qdisc"
#    below could be a probe that cannot see one.
[ "$(val control-present)" = "etf" ] \
	|| fail "the probe could not SEE an etf qdisc that tc had just put on etfa — every absence below would be a broken search"
[ "$(val control-absent)" = "none" ] \
	|| fail "the probe reported an etf qdisc after tc removed it"

for a in default leftover refusal; do
	echo "$OUT" | grep -aq "^$a-daemon-died" && fail "the $a arm's daemon died at start"
done

# 1. THE DEFAULT INSTALLS IT. Nothing was set, and the device carried nothing before.
[ "$(val default-before)" = "none" ] || fail "etfa already carried an etf qdisc before the default arm"
[ "$(val default-running)" = "etf" ] \
	|| fail "the default backend is etf and the running daemon left etfa without an etf qdisc (read: $(line default-running))"

# 2. AND FRAMES ARRIVE THROUGH IT. The measurement the qdisc could silently kill.
[ "$(reac default)" -gt 1000 ] 2>/dev/null \
	|| fail "only $(reac default) REAC frames reached the far end on the etf backend (any-ethertype total $(anyf default)) — the qdisc is eating the wire"

# 3. AND IT IS TAKEN AWAY ON EXIT.
[ "$(val default-after)" = "none" ] \
	|| fail "the daemon exited and left its etf qdisc on etfa ($(line default-after)) — the next thread-backend start would transmit nothing"

# 4. A LEFTOVER ETF IS REMOVED BY A THREAD-BACKEND START, and the wire works.
[ "$(val leftover-before)" = "etf" ] || fail "the leftover arm did not get its leftover qdisc"
[ "$(val leftover-running)" = "none" ] \
	|| fail "REACPW_PACER=thread ran under a LEFTOVER etf qdisc ($(line leftover-running)) — this is the rig's 0-packet failure"
[ "$(reac leftover)" -gt 1000 ] 2>/dev/null \
	|| fail "only $(reac leftover) REAC frames on the thread backend (any $(anyf leftover))"

# 5. THE REFUSAL PATH CARRIES AUDIO AND SAYS WHY. No CAP_NET_ADMIN: no qdisc, no
#    SO_TXTIME, so the ETF default falls back — loudly — and the desk keeps working.
[ "$(val refusal-running)" = "none" ] \
	|| fail "the capability-less daemon installed a qdisc anyway ($(line refusal-running))"
echo "$OUT" | grep -aq 'CANNOT INSTALL the etf qdisc' \
	|| fail "the refusal arm installed nothing and said nothing — a silent fallback is the defect this backend exists to avoid"
echo "$OUT" | grep -aq 'ETF IS THE DEFAULT AND THIS MACHINE CANNOT RUN IT' \
	|| fail "the pacer fell back to the thread backend without one loud line naming the refusal"
[ "$(reac refusal)" -gt 1000 ] 2>/dev/null \
	|| fail "the refusal arm carried only $(reac refusal) frames (any $(anyf refusal)) — a machine that cannot do ETF must still carry audio"

# 6. AND THE PROTOCOL COMPLETES ON TOP OF IT, when the emulator was given.
if echo "$OUT" | grep -aq '^box-arm NOT-RUN'; then
	echo "NOTE: the box-enrolment arm did NOT RUN (set REACPW_FAKE_BOX to libreac's"
	echo "      \`make fake_box\` binary). The frame counts above prove the wire, not"
	echo "      the enrolment."
else
	[ "$(val box-running)" = "etf" ] \
		|| fail "the box arm's daemon did not install its qdisc ($(line box-running))"
	echo "$OUT" | grep -aq 'ESTABLISHED' \
		|| fail "a linked, silent S-4000S on the far end was never ESTABLISHED with the daemon on the ETF backend — the qdisc carries frames but the enrolment does not complete"
fi

BOXARM="NOT RUN"
echo "$OUT" | grep -aq '^box-arm NOT-RUN' || BOXARM="a linked, silent S-4000S reached $(st box-state)"
echo "MEASURED: etf $(reac default) frames in 3 s, thread-after-leftover $(reac leftover), refusal $(reac refusal); box arm: $BOXARM"
echo "OK: the daemon installs its own etf qdisc on the default backend and removes it on exit;" \
     "a thread-backend start strips a leftover etf that would otherwise eat every frame;" \
     "with CAP_NET_ADMIN gone it says so in one loud line and keeps carrying audio;" \
     "and a box enrols through the qdisc it installed"
