#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: A REAC SEGMENT IS A PHYSICAL NIC OR A VLAN ON ONE (#101, libreac's
# reac_ifscan.h). Found 2026-11-15 on a host running rootless containers: the daemon
# listened on and KNOCKED on podman0 (a bridge) and veth0..2 (its container ports), so
# every container on that bridge received REAC control frames.
#
# Two runs over one veth pair in a private user+net+mount+pid namespace, with sysfs
# REMOUNTED inside it so /sys/class/net is this namespace's own (the kernel's own answer
# about v0, not the host's table):
#
#   1. NOT OPTED IN (REAC_IFACES_ALLOW_VIRTUAL unset): v0 is excluded by name with the
#      kernel's reason ("a veth or other virtual interface"), and is never listened on,
#      so nothing is knocked on it.
#   2. CONTROL, opted in (REAC_IFACES_ALLOW_VIRTUAL=v0): the same v0 IS listened on.
#      Without it, run 1's silence could be a daemon that never looked at all.
#
# Every other namespace test opts its veths in through the suite's default setup
# (meson.build); this one clears it for run 1. Skips (77) where the namespace, the sysfs
# remount, iproute2 or PipeWire is unavailable.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v ip >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
unshare -r -n -m -p -f --mount-proc true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

# THIS NAMESPACE'S OWN SYSFS. Without the remount /sys/class/net is the host's table and
# v0 is simply absent from it, which would exclude it for the wrong reason.
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount sysfs in the namespace"; exit 77; }
ip link add v0 type veth peer name v1 || { echo "SKIP: no veth"; exit 77; }
ip link set v0 up && ip link set v1 up
[ -e /sys/class/net/v0 ] || { echo "SKIP: the remounted sysfs does not show v0"; exit 77; }

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || { echo "SKIP: no private PipeWire in this namespace"; exit 77; }

run() {   # run <seconds> [VAR=value ...]: the daemon with nothing configured
	local secs="$1"; shift
	: >"$LOG"
	env -u REAC_IFACES_ALLOW_VIRTUAL "$@" HOME="$CONF" "$BIN" >"$LOG" 2>&1 &
	local pid=$!
	sleep "$secs"
	kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null
}

# 1. Not opted in.
run 3
grep -q "reac-ifscan: v0 is not a REAC segment and is not listened on: a veth" "$LOG" || {
	echo "FAIL: v0 was not excluded by name with the kernel's reason"; tail -15 "$LOG"; exit 1; }
if grep -q "\[v0\] listening\|\[v0\] .*KNOCKING" "$LOG"; then
	echo "FAIL: v0 is a veth and was listened on or knocked on"
	grep "\[v0\]" "$LOG" | head -5; exit 1
fi

# 2. CONTROL: opted in, the same interface is listened on.
run 3 REAC_IFACES_ALLOW_VIRTUAL=v0
grep -q "\[v0\] listening" "$LOG" || {
	echo "FAIL (control): opted in, v0 was still not listened on, so run 1 proves nothing"
	tail -15 "$LOG"; exit 1; }
if grep -q "v0 is not a REAC segment" "$LOG"; then
	echo "FAIL (control): opted in, v0 was still excluded"; exit 1
fi
echo "OK: a veth is excluded by name with the kernel's reason and never listened on;
    opted in by REAC_IFACES_ALLOW_VIRTUAL, the same veth is listened on"
exit 0
INNER
)
rc=$?
echo "$OUT"
exit $rc
