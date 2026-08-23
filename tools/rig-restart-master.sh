#!/bin/bash
# Replace the reac-pw master on ONE segment, safely.
#
# NEVER TWO MASTERS ON A SEGMENT. pkill -f / pgrep -f match the searching shell's
# own command line, so this enumerates /proc and keeps only real thread-group
# leaders (Name == reac-pw and Tgid == pid), then kills EXPLICIT pids and proves
# the segment is clear before starting anything.
#
# The binary loses its file capabilities on every relink, and without them
# reac-pw cannot open a raw socket and fails to sync SILENTLY — so the full set is
# re-applied and verified here rather than remembered. cap_net_admin is needed to
# create VLAN sub-interfaces on a trunk (trunk/VLAN spec §4e); adopting ones that
# already exist needs no capability, which is why the daemon warns rather than
# refuses when only that one is missing.
#
#   tools/rig-restart-master.sh <iface> <binary> <logfile> [extra args...]
set -o pipefail
iface="$1"; bin="$2"; log="$3"; shift 3

masters_on() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ -r "$p/status" ] || continue
		[ "$(grep -m1 '^Name:' "$p/status" | awk '{print $2}')" = "reac-pw" ] || continue
		[ "$(grep -m1 '^Tgid:' "$p/status" | awk '{print $2}')" = "$pid" ] || continue
		tr '\0' ' ' < "$p/cmdline" | grep -q -- "$1" && echo "$pid"
	done
}

for pid in $(masters_on "$iface"); do
	echo "killing master $pid on $iface"
	kill "$pid" 2>/dev/null
done
for i in $(seq 1 50); do
	[ -z "$(masters_on "$iface")" ] && break
	sleep 0.1
done
left=$(masters_on "$iface")
[ -z "$left" ] || { echo "REFUSING TO START: still running on $iface: $left"; exit 1; }
echo "segment $iface clear"

sudo -n setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep "$bin" || exit 1
# getcap normalises the order, so check each capability on its own.
caps=$(getcap "$bin")
for c in cap_net_raw cap_net_admin cap_sys_nice; do
	case "$caps" in
		*"$c"*) ;;
		*) echo "caps INCOMPLETE on $bin: missing $c (got '${caps:-<empty>}') — a nosuid mount strips them silently" >&2; exit 1 ;;
	esac
done

cd "$(dirname "$(dirname "$bin")")" || exit 1
nohup "$bin" --live "$iface" --tx "$iface" "$@" > "$log" 2>&1 &
sleep 3
now=$(masters_on "$iface")
[ -n "$now" ] || { echo "master did not stay up; log:"; tail -20 "$log"; exit 1; }
echo "started pid $now on $iface -> $log"
