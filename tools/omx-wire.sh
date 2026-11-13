#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# THE WIRE TIER ON A REAL CABLE (tag omx-wire). The netns suite proves reac-pw against libreac's fake_box over a veth;
# this runs the same pair over a CABLE between two NICs of one box (omx: reac0 <-> reac1, enp3s0 <-> enp4s0) and soaks
# it: reac-pw (as the console user, with its packaged file capabilities) hunts on one end, fake_box (the stagebox
# emulator) answers on the other, they establish, and tools/omx-wire-probe.py measures every REAC frame both ways for
# SECONDS: one-way latency (both ends stamped by the kernel on one clock) and drops (a frame that left one end and never
# reached the other, and the counter holes at the receiving end).
#
#   sudo tools/omx-wire.sh [-a reac0] [-b reac1] [-s 600] [-u USER] [-o OUTDIR] [--fake PATH] [--reac-pw PATH]
#   sudo tools/omx-wire.sh --veth [-s 20]            # the rehearsal: a veth pair stands in for the cable
#   sudo tools/omx-wire.sh --veth --blip [-s 20]     # sabotage: the box end goes down for 1 s mid-soak; must be RED
#
# Each end goes into a network namespace of its own (omxw-a, omxw-b) for the run and comes back, up, when it ends, by
# whatever road it ends. Nothing else on the box sees the two ports meanwhile, so a reac-pw of the session must not be
# bound to them (the run refuses if one is running).
# Exit: 0 green, 1 red (frames lost, never established, or the link fell out of ESTABLISHED), 2 not measured (no cable,
# not root, a tool missing, the probe lost frames itself), 77 for meson when the tier is not configured on this host.
set -u
A=reac0 B=reac1 SECS=600 USERN=${SUDO_USER:-} OUT= VETH=0 BLIP=0
FAKE=${FAKE_BOX:-/usr/libexec/libreac/tests/fake_box}
BIN=$(command -v reac-pw || echo /usr/bin/reac-pw)
HERE=$(cd "$(dirname "$0")" && pwd)
while [ $# -gt 0 ]; do
	case $1 in
	-a) A=$2; shift ;; -b) B=$2; shift ;; -s) SECS=$2; shift ;; -u) USERN=$2; shift ;; -o) OUT=$2; shift ;;
	--fake) FAKE=$2; shift ;; --reac-pw) BIN=$2; shift ;; --veth) VETH=1 ;; --blip) BLIP=1 ;;
	*) echo "usage: see the head of $0" >&2; exit 2 ;;
	esac
	shift
done
nm() { echo "OMX-WIRE NOT-MEASURED: $*"; exit 2; }
[ "$(id -u)" = 0 ] || nm "run as root (sudo): the two ports move into namespaces"
[ -n "$USERN" ] && [ "$USERN" != root ] && id "$USERN" >/dev/null 2>&1 || nm "name the console user (-u USER): reac-pw refuses root"
[ -x "$FAKE" ] || nm "no fake_box at $FAKE (libreac-tests, or libreac: make fake_box; --fake PATH)"
[ -x "$BIN" ] || nm "no reac-pw at $BIN"
for t in ip setpriv pipewire pw-cli python3; do command -v $t >/dev/null || nm "no $t"; done
pgrep -x reac-pw >/dev/null && nm "a reac-pw is running on this box; stop it first (it would lose $A and $B)"
for n in omxw-a omxw-b; do ip netns list | grep -qw "$n" && nm "netns $n exists (a run in progress, or a stale one: ip netns del $n)"; done

if [ $VETH = 1 ]; then
	A=omxwa0 B=omxwb0
	ip link add $A type veth peer name $B || nm "cannot make a veth pair"
else
	for i in $A $B; do [ -e /sys/class/net/$i ] || nm "no interface $i"; done
	ip link set $A up; ip link set $B up
	for i in $(seq 50); do [ "$(cat /sys/class/net/$A/carrier 2>/dev/null)$(cat /sys/class/net/$B/carrier 2>/dev/null)" = 11 ] && break; sleep 0.1; done
	[ "$(cat /sys/class/net/$A/carrier 2>/dev/null)$(cat /sys/class/net/$B/carrier 2>/dev/null)" = 11 ] ||
		nm "no carrier on $A and $B: cable them to each other"
fi
KEEP=${OUT:-}
OUT=${OUT:-$(mktemp -d /tmp/omx-wire.XXXXXX)}
mkdir -p "$OUT"; RT=$(mktemp -d /tmp/omx-wire-rt.XXXXXX)
mkdir -p "$RT/home"; chown -R "$USERN:" "$RT"; chmod 700 "$RT"

PIDS=
restore() {
	for p in $PIDS; do kill -TERM $p 2>/dev/null; done; sleep 0.5
	for p in $PIDS; do kill -9 $p 2>/dev/null; done
	ip netns pids omxw-a 2>/dev/null | xargs -r kill -9 2>/dev/null; ip netns pids omxw-b 2>/dev/null | xargs -r kill -9 2>/dev/null
	if [ $VETH = 0 ]; then   # the ports come home, up, as the box's own boot left them
		ip -n omxw-a link set $A netns 1 2>/dev/null; ip -n omxw-b link set $B netns 1 2>/dev/null
		ip link set $A up 2>/dev/null; ip link set $B up 2>/dev/null
	fi
	ip netns del omxw-a 2>/dev/null; ip netns del omxw-b 2>/dev/null
	rm -rf "$RT"
}
trap restore EXIT
trap 'exit 1' INT TERM
ip netns add omxw-a && ip netns add omxw-b || nm "cannot make the namespaces"
ip link set $A netns omxw-a && ip link set $B netns omxw-b || nm "cannot move $A/$B into the namespaces"
ip -n omxw-a link set $A up; ip -n omxw-b link set $B up; ip -n omxw-a link set lo up; ip -n omxw-b link set lo up

# THE BOX first, silent until a master pushes a whole scene (the rig's boxes were powered before the daemon)
ip netns exec omxw-b "$FAKE" $B $((SECS + 120)) >"$OUT/box.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
# THE HEARING END, as the console user: a private PipeWire, an EMPTY home (nothing pins a role: the wire is hunted)
ip netns exec omxw-a setpriv --reuid="$USERN" --regid="$USERN" --init-groups \
	env -i PATH=/usr/bin:/bin HOME="$RT/home" XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT" REAC_DEBUG=1 BIN="$BIN" \
	bash -c 'pipewire >"$XDG_RUNTIME_DIR/pw.log" 2>&1 &
	         for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
	         pw-cli info 0 >/dev/null 2>&1 || { echo "no private PipeWire"; exit 2; }
	         exec "$BIN"' >"$OUT/reac-pw.log" 2>&1 &
PIDS="$PIDS $!"
for i in $(seq 300); do grep -q "GRANTING -> ESTABLISHED" "$OUT/reac-pw.log" && break; sleep 0.2; done
if ! grep -q "GRANTING -> ESTABLISHED" "$OUT/reac-pw.log"; then
	echo "--- reac-pw ($A) ---"; tail -20 "$OUT/reac-pw.log"; echo "--- box ($B) ---"; tail -5 "$OUT/box.log"
	echo "OMX-WIRE FAILED: reac-pw on $A never established with the box on $B in 60 s"; exit 1
fi
sleep 2   # the scene push is done; the soak measures the streams
EST_BEFORE=$(grep -c "ESTABLISHED ->" "$OUT/reac-pw.log")
if [ $BLIP = 1 ]; then
	( sleep $((SECS / 2)); ip -n omxw-b link set $B down; sleep 1; ip -n omxw-b link set $B up ) &
	PIDS="$PIDS $!"
fi
echo "omx-wire: $A (reac-pw $("$BIN" --version 2>/dev/null | head -1)) <-> $B (fake_box), soak ${SECS} s, $(uname -r)"
python3 "$HERE/omx-wire-probe.py" omxw-a $A omxw-b $B "$SECS" | tee "$OUT/probe.txt"
RC=${PIPESTATUS[0]}
FELL=$(( $(grep -c "ESTABLISHED ->" "$OUT/reac-pw.log") - EST_BEFORE ))
echo "omx-wire: reac-pw left ESTABLISHED $FELL times during the soak"
[ $RC = 0 ] && [ $FELL -gt 0 ] && { echo "OMX-WIRE FAILED: no frame lost, but the link fell out of ESTABLISHED"; RC=1; }
[ -n "$KEEP" ] && echo "omx-wire: logs in $OUT" || rm -rf "$OUT"
exit $RC
