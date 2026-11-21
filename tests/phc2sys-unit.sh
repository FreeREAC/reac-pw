#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw-phc2sys@.service KEEPS ONE NIC'S PHC ON TAI, AND NEVER FIGHTS A ptp4l FOR IT (reac-pw#98).
# This fails when
#   - the unit's ExecStart carries a literal `-O <number>`: the TAI-UTC offset is the named
#     variable REAC_TAI_UTC_S (default 37, a drop-in overrides it), so a leap second is one edit;
#   - the unit's ExecCondition is a pgrep one-liner: it missed a ptp4l configured by `-f`, and
#     pgrep needs procps, which the package never declared. The check reads /proc itself
#     (packaging/reac-pw-phc2sys-check.sh) and is installed by the spec and debian/;
#   - the check lets a ptp4l through that owns the NIC by `-i <nic>` or by a `[<nic>]` section of
#     its `-f` file, or stops a ptp4l on ANOTHER NIC, or stops when no ptp4l runs.
# A SWEEP THAT CANNOT SEE PROVES IT CAN: each refusal case runs next to its allow case, and a
# case with the check's own match broken (the sabotage arm) must flip.
#
#   tests/phc2sys-unit.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
unit="$ROOT/packaging/reac-pw-phc2sys@.service"
chk="$ROOT/packaging/reac-pw-phc2sys-check.sh"
fail=0
say() { echo "FAIL: $*" >&2; fail=1; }

grep -Eq '^ExecStart=.* -O +[0-9]' "$unit" && say "ExecStart carries a literal -O offset"
grep -Eq '^Environment=REAC_TAI_UTC_S=[0-9]+$' "$unit" || say "no Environment=REAC_TAI_UTC_S=<seconds> default"
grep -Eq '^ExecStart=.* -O +\$\{REAC_TAI_UTC_S\}' "$unit" || say "ExecStart does not take -O from REAC_TAI_UTC_S"
grep -Eq '^ExecCondition=.*pgrep' "$unit" && say "ExecCondition is a pgrep one-liner"
grep -Eq '^ExecCondition=.*reac-pw-phc2sys-check +%I' "$unit" || say "ExecCondition does not run the check with %I"
grep -q 'reac-pw-phc2sys-check' "$ROOT/packaging/reac-pw.spec" || say "the spec does not install the check"
grep -q 'reac-pw-phc2sys-check' "$ROOT/debian/reac-pw.install" || say "debian/reac-pw.install does not install the check"
[ -f "$chk" ] || { say "packaging/reac-pw-phc2sys-check.sh is missing"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
proc() { # <dir> <cmdline words...>: one fake /proc/<pid>/cmdline, NUL separated
	local d="$1" pid=$RANDOM; shift
	mkdir -p "$d/$pid"; printf '%s\0' "$@" >"$d/$pid/cmdline"
}
run() { PHC2SYS_PROC_ROOT="$1" bash "$chk" enp1s0; } # exit 0 = go, non-zero = refuse
printf '[global]\nstep_threshold 1\n[enp1s0]\nserverOnly 1\n' >"$T/owns.conf"
printf '[global]\nstep_threshold 1\n[enp2s0]\nserverOnly 1\n' >"$T/other.conf"

mkdir "$T/none";  run "$T/none"   || say "refused with no ptp4l running"
mkdir "$T/byi";   proc "$T/byi" ptp4l -i enp1s0 -m
run "$T/byi" && say "let a ptp4l -i <nic> through"
mkdir "$T/byf";   proc "$T/byf" ptp4l -f "$T/owns.conf" -m
run "$T/byf" && say "let a ptp4l -f <file with a [<nic>] section> through"
mkdir "$T/oi";    proc "$T/oi" ptp4l -i enp2s0
run "$T/oi" || say "stopped for a ptp4l on another NIC (-i)"
mkdir "$T/of";    proc "$T/of" ptp4l -f "$T/other.conf"
run "$T/of" || say "stopped for a ptp4l on another NIC (-f)"
mkdir "$T/notptp"; proc "$T/notptp" vim -i enp1s0
run "$T/notptp" || say "stopped for a process that is not ptp4l"
# the sabotage arm: the check with its [<nic>] match deleted must let byf through, so byf's refusal above is the match
sed 's/\\\[\$nic\\\]/NEVER/' "$chk" >"$T/sab.sh"
if cmp -s "$chk" "$T/sab.sh"; then say "self-check: the sabotage edit changed nothing"; \
elif PHC2SYS_PROC_ROOT="$T/byf" bash "$T/sab.sh" enp1s0; then :; else say "self-check: the sabotaged check still refused a -f owner"; fi
[ "$fail" = 0 ] && echo "OK: the phc2sys unit names its TAI offset and refuses a ptp4l that owns the NIC, by -i or -f"
exit "$fail"
