#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a master whose frames are NOT LEAVING THE HOST never bounces its port, and
# says why — once. (docs/design/specs/2026-09-16-a-dropped-box-wakes-on-a-phy-edge.md,
# amendment 2026-09-23.)
#
# THE FAULT, ON THE DESK 2026-09-23 13:43 (docs/design/evidence/reac-pw-boot-2026-09-23.log
# lines 54-99, 176-187). The daemon started before chrony had disciplined the clock, the
# kernel's TAI offset was 0, and libreac refused the ETF backend on both masters — under
# the etf root qdisc the daemon had ALREADY installed for it. skip_sock_check drops every
# frame that carries no launch time: tx=0, tx_errors=8000/s, 3.46 M by the end. The master
# still counted "COMPLETED scene push(es)" (it counts what it hands to the pacer, not what
# the kernel launched), so the wake ladder read a fair run of the cheap rung, spent two PHY
# edges on a box that had never heard a master, said "nothing left to try", and the S-0808
# stayed unenrolled for seven minutes until a cable replug re-served the segment with a
# fresh pacer. Nothing in the journal named the fact that WE were the silent party.
#
# THE SHAPE, REPRODUCED WITHOUT A BOX. The refusal is about us, so no box is needed: an
# empty wire, a master probing it, and its frames refused by the kernel.
#
#   HELD ARM.  The daemon runs the thread backend (REACPW_PACER=thread — the arm cannot
#              wait for a machine whose TAI offset is 0, so the same qdisc-versus-backend
#              mismatch is made by hand): three seconds in, an etf root qdisc is added on
#              its device. From then on every sendto() fails. The ladder must HOLD with
#              "our own frames are not leaving the host", say it ONCE, and make NO edge
#              in 40 s — three times its 12 s grace.
#   CONTROL.   Same daemon, no qdisc: an empty wire with frames leaving is exactly the
#              shape the ladder exists for, and it must bounce inside the window, after
#              saying "our own scene push has not completed yet" once and only once.
#
# The injection is proven before the absence is believed: the held arm requires the
# pacer's own line to show tx_errors climbing, or the qdisc never bit and "no edge" is
# not a measurement. Skips (77) where namespaces, iproute2, PipeWire or sch_etf are
# unavailable — module autoload needs privileges this namespace does not have.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

for t in unshare nsenter ip tc pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
grep -q '^sch_etf ' /proc/modules 2>/dev/null || {
	echo "SKIP: sch_etf is not loaded and a user namespace may not autoload it"; exit $SKIP; }
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

SECS="${REACPW_WAKE_SECS:-40}"

run_arm() {   # run_arm <held:0|1> ; prints the arm's transcript
	ARM_HELD="$1" unshare -r -n -m -p -f --mount-proc --map-root-user \
		bash -s -- "$BIN" "$SECS" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; SECS="$2"; HELD="${ARM_HELD:-0}"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# The peer end is another host, up and silent: an empty wire with carrier.
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the peer end"; exit 77; }
in_peer="nsenter -t $NSPID -n -m"
ip link add nsx0 type veth peer name nsxb0 || exit 90
ip link set nsxb0 netns $NSPID || exit 90
ip link set nsx0 up; $in_peer ip link set nsxb0 up

# The qdisc must be addable at all here, or the held arm's absence means nothing.
tc qdisc add dev nsx0 root etf clockid CLOCK_TAI delta 300000 skip_sock_check 2>/dev/null \
	|| { echo "SKIP: this namespace cannot add an etf qdisc at all"; exit 77; }
tc qdisc del dev nsx0 root 2>/dev/null

HOME="$CONF" REACPW_PACER=thread "$BIN" --live nsx0 --tx nsx0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" \
	--name nsx >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || { echo "daemon exited early"; tail -5 "$LOG"; exit 91; }
if [ "$HELD" = 1 ]; then
	tc qdisc add dev nsx0 root etf clockid CLOCK_TAI delta 300000 skip_sock_check \
		|| { echo "qdisc-add-failed"; exit 92; }
	echo "qdisc-added 1"
fi
sleep "$SECS"
kill -TERM $PID 2>/dev/null; sleep 0.5
echo "edges $(grep -ac 'makes the edge itself' "$LOG")"
echo "holds-nothing-sent $(grep -ac "the wake ladder on 'nsx0' holds: our own frames are not leaving the host" "$LOG")"
echo "holds-push-not-proven $(grep -ac "the wake ladder on 'nsx0' holds: our own scene push has not completed yet" "$LOG")"
echo "holds-any $(grep -ac "the wake ladder on 'nsx0' holds:" "$LOG")"
echo "tx-errors-seen $(grep -a 'reac-pacer: ring depth' "$LOG" | sed -n 's/.*tx_errors=\([0-9]*\).*/\1/p' | sort -n | tail -1)"
echo "tx-seen $(grep -a 'reac-pacer: ring depth' "$LOG" | sed -n 's/.*| tx=\([0-9]*\) .*/\1/p' | sort -n | tail -1)"
echo "--- daemon ---"; grep -aE "wake ladder|makes the edge itself|is back up|did not answer|ETF|refused" "$LOG" | tail -12
INNER
}

FAIL=0
say() { printf '%s\n' "$*"; }
val() { echo "$1" | grep -a "^$2 " | head -1 | awk '{print $2}'; }

# ---- HELD ARM: the kernel refuses every frame; the ladder must hold, and say so once.
A1=$(run_arm 1 2>&1) || true
case "$A1" in *"SKIP: "*) echo "${A1##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
say "$A1"
echo "$A1" | grep -q "^qdisc-added 1" || { say "FAIL: the etf qdisc was never added — the injection did not happen"; FAIL=1; }
TXE=$(val "$A1" tx-errors-seen); TX=$(val "$A1" tx-seen)
[ -n "${TXE:-}" ] && [ "$TXE" -gt 1000 ] \
	|| { say "FAIL: tx_errors never climbed (got '${TXE:-}') — the qdisc did not bite, so 'no edge' below is not a measurement"; FAIL=1; }
[ "$(val "$A1" edges)" = "0" ] \
	|| { say "FAIL: the daemon made $(val "$A1" edges) PHY edge(s) on a wire its own frames never reached — the 2026-09-23 boot, two edges spent on a box that had never heard a master"; FAIL=1; }
[ "$(val "$A1" holds-nothing-sent)" = "1" ] \
	|| { say "FAIL: 'our own frames are not leaving the host' was said $(val "$A1" holds-nothing-sent) time(s), not once"; FAIL=1; }

# ---- CONTROL: frames leave; the same empty wire is bounced, after one 'not proven yet'.
A2=$(run_arm 0 2>&1) || true
case "$A2" in *"SKIP: "*) say "SKIP: the control arm could not run"; exit $SKIP;; esac
say "$A2"
[ "$(val "$A2" edges)" -ge 1 ] 2>/dev/null \
	|| { say "FAIL (control): a wire with frames leaving and nobody answering was never bounced — the instrument has shown no edge, so the held arm's zero proves nothing"; FAIL=1; }
[ "$(val "$A2" holds-nothing-sent)" = "0" ] \
	|| { say "FAIL (control): 'not leaving the host' was said on a wire whose frames were leaving"; FAIL=1; }
[ "$(val "$A2" holds-push-not-proven)" = "1" ] \
	|| { say "FAIL (control): 'push has not completed yet' was said $(val "$A2" holds-push-not-proven) time(s) before the edge, not once"; FAIL=1; }
TX2=$(val "$A2" tx-seen)
[ -n "${TX2:-}" ] && [ "$TX2" -gt 1000 ] \
	|| { say "FAIL (control): the pacer reports tx=${TX2:-} — frames were not leaving in the control either"; FAIL=1; }

[ "$FAIL" = 0 ] && say "PASS: frames refused by the kernel hold the ladder (said once, $TXE errors, $TX sent); frames leaving bounce the same wire"
exit $FAIL
