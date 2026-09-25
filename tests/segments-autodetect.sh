#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: the daemon's segments and roles come from the HOST and the WIRE, and the
# only thing that overrides them is reac-pw.conf.
# (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md.)
#
# THE FAULT, MEASURED ON THE DESK 2026-09-16. The rig moved from one box on a 100 Mbit USB
# NIC to three boxes on a 1 Gbit trunk. The console's GENERATED ~/.config/reac-pw/reac-pw.env
# still carried the previous rig's answers, and those answers were now wrong: the VLAN
# segments were pinned `tap`, so the daemon served three mirror ports on a fabric that had
# no mirror. Every segment was up, every node was on the graph, and no audio moved. From
# inside the daemon a stale file and a correct one are indistinguishable, so the fix is not
# a better value — it is that no file decides this at all unless the operator wrote it.
#
# WHAT A UNIT TEST CANNOT SEE HERE, and this is the whole reason for a whole-binary arm:
# tests/test_reac_segconf.c pins the grammar and every one of its rules can be right while
# main.c still asks reac_conf for REAC_ROLE. That is this repo's most expensive shape — a
# green suite over a path the product does not take. So this drives the REAL binary against
# REAL netdevs and reads what it says about each one.
#
#   A. NO conf, and a STALE env carrying the previous rig's pins. Three interfaces —
#      a parent and two VLAN children — must all resolve to `auto`, and the stale keys
#      must be NAMED as ignored rather than silently dropped.
#   B. A conf pins ONE segment `tap`. That one is tap, the others stay auto.
#   C. A conf `ignore`s one segment. It is never sniffed and is named as ignored.
#   D. A tagged VLAN id with NO sub-interface is announced BY ITS ID.
#   E. HOT-PLUG: an interface that appears while the daemon runs becomes a segment with
#      no restart.
#
# PRESENCE BEFORE ABSENCE. Every arm asserts a line it EXPECTS to be there before it
# asserts one is missing: a grep that cannot find anything and a grep over an empty log
# look identical, and this suite has been fooled by that before.
#
# ISOLATION, both halves (tests/declared-vlan-is-minted.sh, same reason): a user+net+pid
# namespace with its own veths AND its own PipeWire on a private runtime dir, because
# `unshare -n` isolates the wire and not the graph. HOME is redirected at the daemon so the
# only conf and env it can read are the ones written here — never the operator's.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

for t in unshare ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
RT=$(mktemp -d)
cleanup() {
	kill -TERM $(jobs -p) 2>/dev/null
	sleep 0.3
	kill -9 $(jobs -p) 2>/dev/null
	rm -rf "$RT"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || { echo "SKIP: no private PipeWire in the namespace"; exit 77; }

CONF="$RT/home"
mkdir -p "$CONF/.config/reac-pw"

# THE TRUNK AND ITS TWO CHILDREN, all three already on the host: a VLAN sub-interface IS
# an interface, so nothing has to mention it for the daemon to find it.
ip link add trunkA type veth peer name farA || exit 90
ip link set trunkA up; ip link set farA up
for v in 11 12; do
	ip link add link trunkA name trunkA.$v type vlan id $v || exit 90
	ip link set trunkA.$v up
done
echo "netdevs $(ip -o link show | awk -F': ' '{print $2}' | tr '\n' ' ')"

# Run the daemon for `secs` with this HOME and echo its transcript, prefixed.
run() {
	# SEPARATE LINES, NOT ONE `local`: a builtin's arguments are all word-expanded
	# BEFORE any of them is assigned, so `local tag=$1 log=$RT/$tag.log` reads $tag
	# unset — and under `set -u` that aborts the arm with no transcript at all.
	local tag="$1"
	local secs="$2"
	local log="$RT/$tag.log"
	HOME="$CONF" "$BIN" >"$log" 2>&1 &
	local pid=$!
	sleep "$secs"
	if ! kill -0 $pid 2>/dev/null; then
		echo "$tag-daemon-died"
		sed "s/^/$tag| /" "$log" | tail -20
		return
	fi
	kill -TERM $pid 2>/dev/null
	for i in $(seq 40); do kill -0 $pid 2>/dev/null || break; sleep 0.2; done
	kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
	sed "s/^/$tag| /" "$log"
}

# ---- A. NO conf, and the PREVIOUS RIG'S env still on disk ---------------------------
cat > "$CONF/.config/reac-pw/reac-pw.env" <<EOF
# what the console generated for the OLD rig, and never revised
REAC_ROLE=auto
REAC_ROLE_trunkA=master
REAC_ROLE_trunkA.11=tap
REAC_ROLE_trunkA.12=tap
EOF
rm -f "$CONF/.config/reac-pw/reac-pw.conf"
run a 3

# ---- B + C. THE ONE OVERRIDE ---------------------------------------------------------
cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
# the operator's own file. reac-pw never writes this.
[segment trunkA.11]
role = tap          ; this wire is a switch mirror

[SEGMENT trunkA.12]
IGNORE = yes

[segment trunkA]
role = nonsense
EOF
run b 3

# ---- D. A TAGGED VLAN ID WITH NO SUB-INTERFACE ---------------------------------------
rm -f "$CONF/.config/reac-pw/reac-pw.conf"
cat > "$CONF/.config/reac-pw/reac-pw.env" <<EOF
# nothing at all about roles
EOF
ip link add link farA name farA.33 type vlan id 33 || exit 90
ip link set farA.33 up
echo "d-before-33 $(ip -o link show trunkA.33 >/dev/null 2>&1 && echo present || echo absent)"
HOME="$CONF" "$BIN" >"$RT/d.log" 2>&1 &
DPID=$!
sleep 2
# THE TAGGED SOURCE IS THE FAR END OF THE VETH, WHICH IS A SEGMENT OF ITS OWN. The daemon
# hears farA.33, finds nothing mastering it and masters it — so REAC frames leave that VLAN
# sub-interface at 8000 a second, TAGGED 33 by the kernel, and arrive on trunkA where no
# trunkA.33 exists. That is precisely the case this arm is about, and it is real REAC
# traffic rather than a synthetic frame.
#
# A HAND-WRITTEN SENDER CANNOT DO THIS JOB HERE, and it is worth saying why rather than
# being rediscovered: every netdev in this namespace is a segment the daemon masters, so it
# carries the daemon's own etf qdisc, and a raw frame with no launch time is refused
# ENOBUFS — measured, 0 of 400 sent. The control below is the frame COUNT in the daemon's
# own verdict line: a detector that saw nothing cannot print one.
sleep 3
echo "d-after-33 $(ip -o link show trunkA.33 >/dev/null 2>&1 && echo present || echo absent)"
kill -TERM $DPID 2>/dev/null
for i in $(seq 40); do kill -0 $DPID 2>/dev/null || break; sleep 0.2; done
kill -9 $DPID 2>/dev/null; wait $DPID 2>/dev/null
sed 's/^/d| /' "$RT/d.log"

# ---- E. HOT-PLUG ----------------------------------------------------------------------
HOME="$CONF" "$BIN" >"$RT/e.log" 2>&1 &
EPID=$!
sleep 2
echo "e-marker ---- the interface appears from here on ----"
ip link add hot0 type veth peer name hot1 || exit 90
ip link set hot0 up; ip link set hot1 up
sleep 3
kill -0 $EPID 2>/dev/null || echo "e-daemon-died"
kill -TERM $EPID 2>/dev/null
for i in $(seq 40); do kill -0 $EPID 2>/dev/null || break; sleep 0.2; done
kill -9 $EPID 2>/dev/null; wait $EPID 2>/dev/null
sed 's/^/e| /' "$RT/e.log"
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

fail() { echo "FAIL: $1"; exit 1; }
# Lines of one arm's transcript.
arm() { echo "$OUT" | grep -a "^$1| "; }
has() { arm "$1" | grep -qaE "$2"; }

for t in a b d e; do
	echo "$OUT" | grep -qa "^$t-daemon-died" && fail "arm $t: the daemon died at start"
done

# 0. THE INSTRUMENT. Every arm must have produced a transcript, or every absence below is
#    a broken search rather than a finding.
for t in a b d e; do
	[ -n "$(arm $t)" ] || fail "arm $t produced no transcript at all — nothing below means anything"
done
has a 'trunkA' || fail "arm a never mentions trunkA: the daemon did not see the interfaces this test made"

# A. NOTHING IS PINNED BY AN ENV KEY, AND THE STALE KEYS ARE NAMED.
for i in trunkA trunkA.11 trunkA.12; do
	has a "\[$i\].*role auto \(autodetected\)" \
		|| fail "arm a: [$i] did not resolve to auto with no conf — a stale REAC_ROLE_<segment> is still deciding this wire: $(arm a | grep -a "\[$i\]" | head -2)"
done
has a 'REAC_ROLE_trunkA\.11.*IGNORED' \
	|| fail "arm a: the stale REAC_ROLE_trunkA.11 was dropped in SILENCE — a key that stopped applying must say so"
# NOT "no `tap` anywhere": the veth PEERS in this namespace hear our own masters and
# elect tap for themselves, correctly. The claim is about the three interfaces this arm
# is about, and it is made per interface above.

# B. THE ONE OVERRIDE, and only where it speaks.
has b '\[trunkA\.11\].*role tap \(reac-pw\.conf\)' \
	|| fail "arm b: [segment trunkA.11] role=tap did not override anything: $(arm b | grep -a 'trunkA\.11' | head -2)"
has b '\[trunkA\].*role auto \(autodetected\)' \
	|| fail "arm b: a conf that names one segment changed another one"

# C. IGNORE removes it, by name.
has b 'trunkA\.12.*IGNORED by reac-pw\.conf' \
	|| fail "arm c: [segment trunkA.12] ignore=yes did not remove it, or removed it silently"
# THE NEGATIVE, WITH THE POSITIVE LINE TAKEN OUT OF IT. The refusal itself says
# "[segment trunkA.12] ignore", so a bare grep for "segment" finds the very line that
# proves the opposite — a search that matches its own answer.
arm b | grep -a 'trunkA\.12' | grep -va 'IGNORED by' | grep -qaE 'listening —|REAC heard|segment up' \
	&& fail "arm c: an IGNORED segment was still listened on or served: $(arm b | grep -a 'trunkA\.12' | grep -va 'IGNORED by' | head -2)"

# C2. A REFUSAL IS BY NAME. `role = nonsense` must be reported and must not stop the daemon.
has b 'nonsense' \
	|| fail "arm c: an unparsable role value was swallowed — a typo in this file must be visible"

# D. A VLAN ID WITH NO SUB-INTERFACE IS ANNOUNCED BY ITS ID.
[ "$(echo "$OUT" | grep -a '^d-before-33' | awk '{print $2}')" = "absent" ] \
	|| fail "arm d: trunkA.33 existed before the daemon ran"
has d 'vid 33' \
	|| fail "arm d: a tagged VLAN 33 was heard on the trunk and its id was never named: $(arm d | grep -a 'trunkA\]' | tail -3)"
arm d | grep -a 'vid 33' | grep -qaE '\(([1-9][0-9]*) frame' \
	|| fail "arm d: the trunk named vid 33 with NO frame count — a detector that saw nothing cannot have named it: $(arm d | grep -a 'vid 33' | head -1)"
[ "$(echo "$OUT" | grep -a '^d-after-33' | awk '{print $2}')" = "present" ] \
	|| fail "arm d: vid 33 was named and no sub-interface was made for it, and no refusal said why"

# E. HOT-PLUG, with no restart.
has e 'hot0' \
	|| fail "arm e: an interface that appeared while the daemon ran was never noticed"

echo "OK"
exit 0
