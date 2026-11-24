#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The PACKAGED SHAPE, as shipped text: the unit gives the daemon no flags
# (ExecStart=/usr/bin/reac-pw) and nothing is configured for it, and the preset enables it for
# the console user. That the BINARY survives that shape (--help, a bare run) is
# tests/cli-front-door.sh; this reads the two shipped files, which are the product.
#
# AND THE UNIT DOES NOT SECOND-GUESS THE CONF FILE (#100). A per-segment key on a
# VLAN carries a DOT (`REAC_ROLE_enp131s0.11`), which systemd refuses as an
# environment variable name: with `EnvironmentFile=` present it logged
# `Ignoring invalid environment assignment` once per VLAN on every start, over
# pins the daemon had in fact applied from the very same file. The daemon is that
# file's reader; the unit must not be a second one. The role keys that carried the dot
# are retired (spec 2026-09-16) and the ONE override is reac-pw.conf, which systemd could
# not parse at all — so the rule holds for every remaining per-segment key and now also
# for a file that is not an environment file in the first place.
set -u
UNIT="${1:?usage: packaged-shape-starts.sh /path/to/reac-pw.service /path/to/90-reac-pw.preset}"
PRESET="${2:?usage: packaged-shape-starts.sh /path/to/reac-pw.service /path/to/90-reac-pw.preset}"

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

# ---- the preset, if it was handed to us --------------------------------------
# 1.0.8: the RPM ships /usr/lib/systemd/user-preset/90-reac-pw.preset so a fresh
# install starts the unit for the console user. PRESENCE BEFORE ABSENCE again —
# check the file parses as a preset (a bare `enable`/`disable` line) before
# trusting which unit it names.
if [ -n "$PRESET" ]; then
  [ -r "$PRESET" ] || { echo "FAIL: cannot read the preset file '$PRESET'"; exit 1; }
  grep -qE '^(enable|disable)[[:space:]]' "$PRESET" || {
    echo "FAIL: '$PRESET' has no enable/disable line — this reader cannot see a preset"
    echo "      directive at all, so its verdict on reac-pw.service below means nothing"; exit 1; }
  grep -qE '^enable[[:space:]]+reac-pw\.service[[:space:]]*$' "$PRESET" || {
    echo "FAIL: '$PRESET' does not 'enable reac-pw.service' — a fresh install would not"
    echo "      start the daemon for the console user"; exit 1; }
fi

echo "OK: the unit carries no EnvironmentFile= and the preset enables reac-pw.service"
