#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# INTEGRATION: a daemon whose --live interface goes away must EXIT, so the
# service manager can restart it onto the live one. Detection alone is not
# recovery — that was the expensive half of the 2026-08-29 S-0808 outage.
#
# Two scenarios, because they are DIFFERENT FAULTS and only one of them was
# ever caught:
#
#   1. REMOVED  — the name stops resolving. The old name-only check caught this.
#   2. REPLACED — the interface comes back under the SAME NAME with a new
#      ifindex (a USB NIC re-enumerating: the AX88179 on 2026-08-29, unplugged
#      22:14:20, back 22:17:00, same name AND same MAC). A name-only check calls
#      this HEALTHY and falls silent while the socket is deaf and mute. Measured
#      against the pre-fix binary: zero warnings, still running.
#
# Runs entirely inside an unprivileged user+net namespace, so it creates and
# destroys only its own dummy interfaces and cannot touch a live REAC segment.
# Skips (77) where that namespace or PipeWire is unavailable.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
unshare -r -n --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without the
# dummy driver is a machine this test cannot run on, and says so here, so a later `|| exit 90`
# is a FAIL.
unshare -r -n sh -c 'ip link add d0 type dummy' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a dummy link in a namespace (no dummy driver)"; exit $SKIP; }

# The body runs INSIDE the namespace; $1 picks the scenario.
run_case() {
	unshare -r -n --map-root-user bash -s -- "$BIN" "$1" <<'INNER'
set -u
BIN="$1"; MODE="$2"
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

ip link add vanish0 type dummy && ip link set vanish0 up || exit 90
ip link add spacer0 type dummy || exit 90   # burn an index so a recreated
                                            # vanish0 cannot reuse the old one
"$BIN" --live vanish0 --tx vanish0 --mixer m5000 --rate "$FACT_SAMPLE_RATE_96K" >"$LOG" 2>&1 &
PID=$!
sleep 5
if ! kill -0 $PID 2>/dev/null; then
	wait $PID; rc=$?
	# A DAEMON THAT DIES AT START IS A FAIL, NOT A SKIP (audit 2026-09-24, H3). This body
	# starts no PipeWire and the daemon runs without one (it is how a boot before the
	# graph starts), so nothing about the machine explains this exit.
	echo "daemon-died at start (exit $rc)"
	tail -3 "$LOG"
	exit 91
fi

ip link delete vanish0
if [ "$MODE" = replaced ]; then
	ip link delete spacer0
	ip link add vanish0 type dummy && ip link set vanish0 up
fi

sleep 6
if kill -0 $PID 2>/dev/null; then
	echo "FAIL: still running after the interface was $MODE"
	# Show whatever it DID say — a pre-fix binary logs the old VANISHED alarm on
	# the removed case and is completely silent on the replaced one, and that
	# difference is the point of the second scenario.
	grep -hE "IS GONE|WAS REPLACED|VANISHED" "$LOG" | tail -1 | sed 's/^/  said: /' \
		|| echo "  (and it said nothing about it)"
	kill -9 $PID 2>/dev/null
	exit 1
fi
wait $PID; rc=$?
grep -hE "IS GONE|WAS REPLACED" "$LOG" | sed 's/^/  /'
[ $rc -ne 0 ] || { echo "FAIL: exited 0 — a lost interface is a FAILURE exit"; exit 1; }
echo "  exited $rc"
exit 0
INNER
}

fails=0
for mode in removed replaced; do
	echo "--- interface $mode ---"
	run_case "$mode"; rc=$?
	case $rc in
		0)  echo "  OK" ;;
		77) echo "SKIP: environment cannot run the $mode case"; exit $SKIP ;;
		*)  echo "  FAILED"; fails=$((fails + 1)) ;;
	esac
done

[ $fails -eq 0 ] || { echo "$fails scenario(s) failed"; exit 1; }
echo "OK: a removed AND a same-name-replaced --live interface both terminate the daemon"
