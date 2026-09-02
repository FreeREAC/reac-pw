#!/bin/bash
# The PACKAGED SHAPE: `reac-pw` with NO arguments must try to START — hearing its
# segments on every linked interface — and never answer with the usage text.
#
# This is the shape the unit gives the daemon (ExecStart=/usr/bin/reac-pw, no
# flags), and nothing is configured for it: REAC_IFACES retired with the
# per-interface files (openmixer's trunk-VLAN spec, amendment 2026-09-02). An
# `argc < 2` guard once answered usage before anything else and made the
# packaged service unstartable; measured 2026-08-31 on the rig.
#
# The test does NOT need a NIC or a capability. It asserts the two ends of the
# decision:
#   - `--help` still explains itself, exit 0, before any preflight;
#   - bare `reac-pw` gets PAST argument handling: whatever stops it next (the
#     capability preflight here, PipeWire on a host with none), it is not exit 2
#     with the usage text.
set -u
BIN="${1:?usage: packaged-shape-starts.sh /path/to/reac-pw}"

out=$(HOME=/nonexistent-reac-pw-test "$BIN" --help 2>&1); rc=$?
if [ "$rc" -ne 0 ] || ! grep -q "usage:" <<<"$out"; then
  echo "FAIL: --help must print usage and exit 0 (rc=$rc)"; exit 1
fi
if ! grep -q "HEARS its segments" <<<"$out"; then
  echo "FAIL: the usage text no longer describes the packaged shape"; exit 1
fi

out=$(HOME=/nonexistent-reac-pw-test timeout 5 "$BIN" 2>&1); rc=$?
if [ "$rc" -eq 2 ] && grep -q "usage:" <<<"$out"; then
  echo "FAIL: bare reac-pw answered usage — the packaged shape is unreachable"; echo "$out" | head -3; exit 1
fi

echo "OK: the packaged (no-argument) shape starts without a declaration"
