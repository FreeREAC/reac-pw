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
#
# AND THE UNIT DOES NOT SECOND-GUESS THE CONF FILE (#100). A per-segment key on a
# VLAN carries a DOT (`REAC_ROLE_enp131s0.11`), which systemd refuses as an
# environment variable name: with `EnvironmentFile=` present it logged
# `Ignoring invalid environment assignment` once per VLAN on every start, over
# pins the daemon had in fact applied from the very same file. The daemon is that
# file's reader; the unit must not be a second one.
set -u
BIN="${1:?usage: packaged-shape-starts.sh /path/to/reac-pw [/path/to/reac-pw.service]}"
UNIT="${2:-}"

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

# ---- the unit file, if it was handed to us -----------------------------------
if [ -n "$UNIT" ]; then
  [ -r "$UNIT" ] || { echo "FAIL: cannot read the unit file '$UNIT'"; exit 1; }
  # PRESENCE BEFORE ABSENCE: prove this reader can see a directive that MUST be
  # there before trusting it about one that must not. A grep over an empty or
  # misspelt path reports the same clean absence as a fixed unit.
  grep -qE '^ExecStart=' "$UNIT" || {
    echo "FAIL: the unit has no ExecStart= — this reader cannot see directives at all,"
    echo "      so its verdict on EnvironmentFile= below would mean nothing"; exit 1; }
  if grep -qE '^EnvironmentFile=' "$UNIT"; then
    echo "FAIL: the unit still carries EnvironmentFile= (#100). systemd cannot parse a"
    echo "      per-segment key with a dot in it, and the daemon already reads that file."
    grep -nE '^EnvironmentFile=' "$UNIT"; exit 1
  fi
fi

echo "OK: the packaged (no-argument) shape starts without a declaration"
