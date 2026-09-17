#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE DAEMON REFUSES TO START AS THE HOST'S REAL ROOT — AND ONLY THAT ROOT.
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
# — the r1 build container runs meson test as the container's real root by default (no
# unshare in the way), so on r1 the bare run below IS the refusal case; on an
# unprivileged desk shell it is the trivial early-return case. Either way the assertion
# is derived from a measurement, not a guess about the caller.
set -u
BIN="${1:?usage: refuses-root.sh /path/to/reac-pw}"
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
	echo "OK: host root (uid_map $my_map) refused, rc=$rc_bare; namespaced root (uid_map $map_fake) not refused"
else
	if grep -q "E_ROOT_REFUSED" <<<"$out_bare"; then
		echo "FAIL: this shell is NOT the host's root (uid $(id -u), uid_map: $my_map)"
		echo "      and was refused anyway — the check is not reading the identity"
		echo "      it claims to"
		exit 1
	fi
	echo "OK: unprivileged uid $(id -u) not refused; namespaced root (uid_map $map_fake) not refused"
fi
