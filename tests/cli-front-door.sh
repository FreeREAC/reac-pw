#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COMMAND LINE'S FRONT DOOR: what reac-pw says and does to the arguments it is given,
# before it opens a socket or a PipeWire context. One script, one empty namespace per arm:
#
#   1. it refuses to start as the host's real root, and only that root (the long story
#      below);
#   2. `--help` explains itself and exits 0, and a bare `reac-pw` (the packaged service's
#      shape) gets PAST argument handling — never usage and exit 2 (measured on the rig,
#      2026-08-31: an `argc < 2` guard made the packaged service unstartable);
#   3. an unknown flag prints usage and exits 2 (the control for arm 4);
#   4. `--set KEY=VALUE` reaches the daemon: a known knob is accepted and announced at the
#      cli layer, an unknown key is refused by code before anything opens. `--set` fell
#      through the flag loop into usage() for three days, because its only test called the
#      resolver and never ran the binary (found 2026-09-20).
#
# The three scripts this replaces (refuses-root, set-flag-reaches-the-daemon, and the
# `--help` and bare-run half of packaged-shape-starts) each ran reac-pw once or twice with odd
# arguments in an empty namespace. The unit and preset text checks of packaged-shape-starts
# stay where they were: they read shipped files, not the binary.
#
# ---- 1. THE DAEMON REFUSES TO START AS THE HOST'S REAL ROOT — AND ONLY THAT ROOT.
#
# The defect (2026-09-18): `sudo dnf install reac-pw-1.0.18` globally enabled the
# packaged unit (packaging bug, fixed the same release), and the next `sudo`-spawned
# root user manager started a SECOND reac-pw — under root, a DIFFERENT systemd --user
# instance than the console session's, sharing the host's network namespace. It bound
# the same abstract segment-lock socket (abstract AF_UNIX sockets are scoped by NETWORK
# namespace) and won it, and the console user's own daemon logged "the segment is
# held" while every box vanished. A root instance is never the console's, so main() now
# refuses to even try, before it opens anything.
#
# NOT EVERY uid 0 IS THAT ROOT, and this is the harder half of the proof. Every
# capability-gated whole-binary test under tests/ (25 of them) runs this same binary
# through `unshare -r --map-root-user` — a fake root, mapped from ONE unprivileged host
# uid inside a NEW user namespace, so it can hold CAP_NET_RAW/CAP_NET_ADMIN over a
# private veth pair with no real host root anywhere. geteuid() reads 0 there too. A
# check that refused every uid 0 would refuse those 25 tests along with the one process
# it exists for — so the real assertion here is the EXCEPTION, not just the refusal.
#
# The two are told apart by /proc/self/uid_map (src/main.c,
# uid_map_is_full_host_range()): the host's own namespace maps the full 32-bit range,
# "0 0 4294967295", because nothing remapped it; `unshare -r` always narrows that to a
# single line of length 1. PRESENCE BEFORE ABSENCE: this script reads its OWN uid_map
# first and asserts against what it actually is, rather than assuming "not run as root"
# — a remote build container runs meson test as the container's real root by default (no
# unshare in the way), so there the bare run below IS the refusal case; on an
# unprivileged desk shell it is the trivial early-return case. Either way the assertion
# is derived from a measurement, not a guess about the caller.
set -u
BIN="${1:?usage: cli-front-door.sh /path/to/reac-pw}"
SKIP=77

command -v unshare >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
unshare -r -f --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user namespaces unavailable"; exit $SKIP; }

# ---- the exception: fake (namespaced) root must NOT be refused ---------------
out_fake=$(unshare -r -f --map-root-user bash -c \
	'HOME=/nonexistent-reac-pw-test timeout 5 "$1"' _ "$BIN" 2>&1)
map_fake=$(unshare -r -f --map-root-user cat /proc/self/uid_map 2>/dev/null)
if ! grep -qE '^\s*0\s+[0-9]+\s+1\s*$' <<<"$map_fake"; then
	echo "FAIL: this probe's own control is broken — unshare -r --map-root-user did"
	echo "      not produce the narrow uid_map this whole test depends on:"
	echo "$map_fake"
	exit 1
fi
if grep -q "E_ROOT_REFUSED" <<<"$out_fake"; then
	echo "FAIL: fake (namespaced) root was refused. This breaks every one of the 25"
	echo "      capability-gated whole-binary tests under tests/, which all rely on"
	echo "      unshare -r --map-root-user for raw sockets with no real host root."
	echo "$out_fake" | head -10
	exit 1
fi

# ---- the bare run: expectation depends on what identity we actually have -----
my_map=$(cat /proc/self/uid_map 2>/dev/null || echo "")
i_am_host_root=0
if [ "$(id -u)" = "0" ] && grep -qE '^\s*0\s+0\s+4294967295\s*$' <<<"$my_map"; then
	i_am_host_root=1
fi

out_bare=$(HOME=/nonexistent-reac-pw-test timeout 5 "$BIN" 2>&1); rc_bare=$?

if [ "$i_am_host_root" -eq 1 ]; then
	if ! grep -q "E_ROOT_REFUSED" <<<"$out_bare"; then
		echo "FAIL: this shell IS the host's root (uid_map: $my_map) and the daemon"
		echo "      was not refused — root can still start it"
		echo "$out_bare" | head -20
		exit 1
	fi
	[ "$rc_bare" -eq 1 ] || { echo "FAIL: the root refusal must exit 1 (got $rc_bare)"; exit 1; }
	if grep -qE "CAP_NET_RAW|CapEff" <<<"$out_bare"; then
		echo "FAIL: the capability preflight ran before the root refusal — uid 0"
		echo "      carries every capability, so that ordering hides this check"
		exit 1
	fi
	echo "ok: host root (uid_map $my_map) refused, rc=$rc_bare; namespaced root (uid_map $map_fake) not refused"
else
	if grep -q "E_ROOT_REFUSED" <<<"$out_bare"; then
		echo "FAIL: this shell is NOT the host's root (uid $(id -u), uid_map: $my_map)"
		echo "      and was refused anyway — the check is not reading the identity"
		echo "      it claims to"
		exit 1
	fi
	echo "ok: unprivileged uid $(id -u) not refused; namespaced root (uid_map $map_fake) not refused"
fi

# ---- 2. --help and the packaged (no-argument) shape ---------------------------------------
FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
BIN=$(readlink -f "$BIN")

out=$(HOME=/nonexistent-reac-pw-test "$BIN" --help 2>&1); rc=$?
if [ "$rc" -ne 0 ] || ! grep -q "usage:" <<<"$out"; then
	fail "--help must print usage and exit 0 (rc=$rc)"
fi
grep -q "HEARS its segments" <<<"$out" \
	|| fail "the usage text no longer describes the packaged shape"

# The bare run needs no capability to be asked this question: whatever stops it next (the
# capability preflight here, PipeWire on a host with none), it is not usage and exit 2.
out=$(HOME=/nonexistent-reac-pw-test timeout 5 "$BIN" 2>&1); rc=$?
if [ "$rc" -eq 2 ] && grep -q "usage:" <<<"$out"; then
	fail "bare reac-pw answered usage — the packaged shape is unreachable"
	echo "$out" | head -3
fi

# ---- 3. THE CONTROL for arm 4, and it comes first: this probe has to be able to SEE the
#         defect it is named for. An unknown flag must produce the usage text and exit 2 —
#         exactly what `--set` produced for three days.
unshare -r -n true 2>/dev/null || { echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }
OUT=$(unshare -r -n env HOME=/nonexistent "$BIN" --nope 2>&1); rc=$?
[ "$rc" = "2" ] || fail "an unknown flag did not exit 2 (got $rc) — this probe cannot tell a refused flag from an accepted one, so nothing below is a measurement"
echo "$OUT" | grep -q "^usage:" \
	|| fail "an unknown flag printed no usage text — the probe's discriminator does not discriminate"

# ---- 4a. A KNOWN KNOB IS ACCEPTED BY THE FLAG LOOP and announced at the cli layer. The
#          daemon is killed after a few seconds: what is asserted is that it got PAST
#          argument parsing, not what it did next.
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
unshare -r -n env HOME="$D" timeout 5 "$BIN" --set REAC_DEBUG=1 --pcap /nonexistent.pcap \
	>"$D/set.log" 2>&1
if grep -q "^usage:" "$D/set.log"; then
	fail "--set REAC_DEBUG=1 was refused by the flag loop — the flag --help documents does not reach the daemon (2026-09-20)"
	head -3 "$D/set.log" | sed 's/^/  /'
fi
grep -qa "S_KNOB_SET knob REAC_DEBUG=1 (cli)" "$D/set.log" \
	|| { fail "--set did not announce its knob at the cli layer"; head -5 "$D/set.log" | sed 's/^/  /'; }

# ---- 4b. AND AN UNKNOWN KNOB IS STILL REFUSED, with its code, before anything opens.
OUT=$(unshare -r -n env HOME=/nonexistent "$BIN" --set NOPE=1 --pcap /dev/null 2>&1); rc=$?
[ "$rc" = "2" ] || fail "--set with an unknown key did not exit 2 (got $rc)"
echo "$OUT" | grep -q "E_UNKNOWN_KNOB" \
	|| fail "--set with an unknown key did not refuse by code"

[ $FAIL -eq 0 ] || exit 1
echo "PASS: root refused (and only real root); --help and the bare packaged shape get past argument handling; an unknown flag is usage; --set reaches the daemon and still refuses an unknown key"
