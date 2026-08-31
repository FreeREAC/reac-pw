#!/bin/bash
# The PACKAGED SHAPE: `reac-pw` with NO arguments must read REAC_IFACES from the
# layered conf and try to start, not print usage and exit.
#
# This is the shape auto-spine §5 gives the unit, and it was unreachable: an
# `argc < 2` guard answered before the conf was ever consulted, so the packaged
# service could never start and the config-once design was dead on arrival.
# Measured 2026-08-31 on the rig while retiring the hand-run.
#
# The test does NOT need a NIC. It asserts the two ends of the decision:
#   - with nothing configured, bare `reac-pw` still explains itself (exit 2);
#   - with REAC_IFACES naming an interface, it gets PAST argument handling —
#     it must not exit 2 with the usage text.
set -u
BIN="${1:?usage: packaged-shape-starts.sh /path/to/reac-pw}"

out=$(HOME=/nonexistent-reac-pw-test "$BIN" 2>&1); rc=$?
if [ "$rc" -ne 2 ] || ! grep -q "usage:" <<<"$out"; then
  echo "FAIL: with no conf, bare reac-pw must still print usage and exit 2 (rc=$rc)"; exit 1
fi

# A name no host has: the daemon must get past the argument stage and fail on the
# INTERFACE, which is a different answer with a different exit path.
out=$(HOME=/nonexistent-reac-pw-test REAC_IFACES=reacpw-no-such-if "$BIN" 2>&1); rc=$?
if [ "$rc" -eq 2 ] && grep -q "usage:" <<<"$out"; then
  echo "FAIL: REAC_IFACES was ignored — the packaged shape is unreachable"; echo "$out" | head -3; exit 1
fi

echo "OK: the packaged (no-argument) shape reads REAC_IFACES"
