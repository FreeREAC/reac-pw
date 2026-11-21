#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw-phc2sys@<nic>.service's ExecCondition (reac-pw#98): exit 0 when no ptp4l owns <nic>'s
# PTP hardware clock, non-zero (the unit is skipped, not failed) when one does. Two writers on
# one PHC fight, and where ptp4l runs (AES67) it owns the PHC.
#
# A ptp4l owns <nic> when its command line names it, `-i <nic>` (or `-i<nic>`), or when its
# `-f <file>` has a `[<nic>]` section: a ptp4l started by a config file carries no -i at all,
# which the pgrep line this replaces could not see. Read from /proc directly, so the package
# needs no procps. PHC2SYS_PROC_ROOT points the test at a made-up /proc.
#
#   reac-pw-phc2sys-check.sh <nic>
set -u
nic="${1:?usage: reac-pw-phc2sys-check.sh <nic>}"
proc="${PHC2SYS_PROC_ROOT:-/proc}"

for cl in "$proc"/[0-9]*/cmdline; do
	[ -r "$cl" ] || continue
	args=()
	while IFS= read -r -d '' a; do args+=("$a"); done <"$cl"
	[ "${#args[@]}" -gt 0 ] || continue
	[ "$(basename -- "${args[0]}")" = ptp4l ] || continue
	for ((i = 1; i < ${#args[@]}; i++)); do
		case "${args[i]}" in
		-i)  [ "${args[i+1]:-}" = "$nic" ] && exit 1 ;;
		-i*) [ "${args[i]#-i}" = "$nic" ] && exit 1 ;;
		-f)  f="${args[i+1]:-}"
		     [ -r "$f" ] && grep -qx "\[$nic\]" "$f" && exit 1 ;;
		esac
	done
done
exit 0
