#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# A SLAVE DECLARES A CATALOGUE ROW, NEVER A BARE CHANNEL COUNT.
#
# `--box-channels N` names the box this daemon presents as by its input width. A width no
# captured catalogue row has used to be accepted and answered with the S-1608's declaration;
# it is refused where it is read, with the ordinary usage error (exit 2) and a sentence that
# says why. A width a row has is not refused for that reason.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
fail=0
# The daemon's capability preflight runs before its arguments are read, so it runs in a user
# and network namespace of its own making, where the capability it checks for is held.
unshare -Urn true 2>/dev/null || { echo "SKIP: no user+net namespace here"; exit 77; }
RUN=(unshare -Urn timeout 10 "$BIN")

out=$("${RUN[@]}" --live nosuchif0 --tx nosuchif0 --role slave --box-channels 24 2>&1)
rc=$?
[ "$rc" = 2 ] || { echo "FAIL: --box-channels 24 exited $rc, not the usage error 2: $out"; fail=1; }
echo "$out" | grep -q "catalogue row" || { echo "FAIL: --box-channels 24 was not refused for lacking a catalogue row: $out"; fail=1; }

# the control: a width a row has is not refused for that reason (the run itself ends at
# the interface that does not exist, which is not what is asked here)
out=$("${RUN[@]}" --live nosuchif0 --tx nosuchif0 --role slave --box-channels 16 2>&1)
echo "$out" | grep -q "catalogue row" && { echo "FAIL: --box-channels 16 (the S-1608) was refused for lacking a row: $out"; fail=1; }

[ "$fail" = 0 ] && echo "OK: --box-channels must name a width a catalogue row has"
exit $fail
