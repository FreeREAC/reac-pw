#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a master on a NIC THE HOST USES never bounces that NIC, and says why,
# once (#46, 2026-10-26-a-host-nic-is-never-bounced-or-streamed-at).
#
# THE FAULT, ON THE DESK 2026-10-26 19:19Z. The wake ladder (reac_wake.h) took the trunk
# parent down for 1200 ms: the NIC that carries the desk's management address and every
# REAC VLAN. The edge is a host network change; every browser tab on the host dropped
# every stream, the console page re-dialled, and a build that ran all afternoon made that
# edge every 17 s for four hours. The ladder's premise (a box that DROPPED waits on a PHY
# edge) holds for a NIC that is a REAC wire and nothing else, and the kernel says which
# NICs are not (reac_link_owner.h: an address beyond link scope, a stacked sub-interface,
# a default route).
#
# THE SHAPE, REPRODUCED WITHOUT A BOX, on the same two arms as wake-holds-when-nothing-is-sent:
#
#   HELD ARM.  The daemon's own NIC carries 10.99.2.1/24 before the daemon starts. The
#              ladder must HOLD with "this interface is the host's", say it ONCE, say
#              nothing else, and make NO edge in 40 s, three times its 12 s grace.
#   CONTROL.   Same daemon, no address (the veth carries only the kernel's fe80): the
#              empty wire the ladder exists for, and it must bounce inside the window
#              after saying "our own scene push has not completed yet" once.
#
# The held arm's frames must be LEAVING (tx climbing), or "no edge" is NOTHING_SENT's
# doing and not this refusal's. Skips (77) where namespaces, iproute2 or PipeWire are
# unavailable.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
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
if [ "$HELD" = 1 ]; then
	ip addr add 10.99.2.1/24 dev nsx0 || { echo "addr-add-failed"; exit 92; }
	echo "addr-added 1"
fi

HOME="$CONF" REACPW_PACER=thread "$BIN" --live nsx0 --tx nsx0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" \
	--name nsx >"$LOG" 2>&1 &
PID=$!
sleep 3
kill -0 $PID 2>/dev/null || { echo "daemon exited early"; tail -5 "$LOG"; exit 91; }
sleep "$SECS"
kill -TERM $PID 2>/dev/null; sleep 0.5
echo "edges $(grep -ac 'makes the edge itself' "$LOG")"
echo "holds-host-nic $(grep -ac "the wake ladder on 'nsx0' holds: this interface is the host's" "$LOG")"
echo "holds-host-nic-addr $(grep -ac "the kernel says it is the host's: it carries an address beyond link scope" "$LOG")"
echo "holds-push-not-proven $(grep -ac "the wake ladder on 'nsx0' holds: our own scene push has not completed yet" "$LOG")"
echo "holds-any $(grep -ac "the wake ladder on 'nsx0' holds:" "$LOG")"
echo "tx-seen $(grep -a 'reac-pacer: ring depth' "$LOG" | sed -n 's/.*| tx=\([0-9]*\) .*/\1/p' | sort -n | tail -1)"
echo "--- daemon ---"; grep -aE "wake ladder|makes the edge itself|is back up|did not answer|refused" "$LOG" | tail -8
INNER
}

FAIL=0
say() { printf '%s\n' "$*"; }
val() { echo "$1" | grep -a "^$2 " | head -1 | awk '{print $2}'; }

# ---- HELD ARM: the NIC is the host's; the ladder must hold, say so once, and nothing else.
A1=$(run_arm 1 2>&1) || true
case "$A1" in *"SKIP: "*) echo "${A1##*SKIP: }" | head -1 | sed 's/^/SKIP: /'; exit $SKIP;; esac
say "$A1"
echo "$A1" | grep -q "^addr-added 1" || { say "FAIL: the address was never added — the injection did not happen"; FAIL=1; }
TX=$(val "$A1" tx-seen)
[ -n "${TX:-}" ] && [ "$TX" -gt 1000 ] \
	|| { say "FAIL: the pacer reports tx=${TX:-} — frames were not leaving, so 'no edge' is NOTHING_SENT's and not this refusal's"; FAIL=1; }
[ "$(val "$A1" edges)" = "0" ] \
	|| { say "FAIL: the daemon made $(val "$A1" edges) PHY edge(s) on a NIC that carries the host's address — the desk's trunk parent, 2026-10-26"; FAIL=1; }
[ "$(val "$A1" holds-host-nic)" = "1" ] \
	|| { say "FAIL: 'this interface is the host's' was said $(val "$A1" holds-host-nic) time(s), not once"; FAIL=1; }
[ "$(val "$A1" holds-host-nic-addr)" = "1" ] \
	|| { say "FAIL: the kernel's own reason (an address beyond link scope) was not named"; FAIL=1; }
[ "$(val "$A1" holds-any)" = "1" ] \
	|| { say "FAIL: $(val "$A1" holds-any) refusals were said on the host's NIC; the host's NIC is the one fact, said once"; FAIL=1; }

# ---- CONTROL: a bare veth; the same empty wire is bounced, after one 'not proven yet'.
A2=$(run_arm 0 2>&1) || true
case "$A2" in *"SKIP: "*) say "SKIP: the control arm could not run"; exit $SKIP;; esac
say "$A2"
[ "$(val "$A2" edges)" -ge 1 ] 2>/dev/null \
	|| { say "FAIL (control): a bare wire with frames leaving and nobody answering was never bounced — the instrument has shown no edge, so the held arm's zero proves nothing"; FAIL=1; }
[ "$(val "$A2" holds-host-nic)" = "0" ] \
	|| { say "FAIL (control): 'the host's' was said on a veth that carries only fe80"; FAIL=1; }
[ "$(val "$A2" holds-push-not-proven)" = "1" ] \
	|| { say "FAIL (control): 'push has not completed yet' was said $(val "$A2" holds-push-not-proven) time(s) before the edge, not once"; FAIL=1; }

[ "$FAIL" = 0 ] && say "PASS: an address on the NIC holds the ladder (said once, $TX frames sent, no edge); the bare wire is bounced"
exit $FAIL
