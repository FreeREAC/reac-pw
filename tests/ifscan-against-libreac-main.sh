#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# test_reac_ifscan, BUILT AGAINST LIBREAC MAIN (reac-pw#124).
#
# The test feeds the scan netlink datagrams for interfaces it makes up. libreac's scan admits
# only a physical NIC or a VLAN on one (the gate, in main, not in the 1.11.0 package), and the
# test opens it with reac_ifscan_set_allow_virtual, which only main declares. So the test is
# compiled here from libreac's SOURCE, the sibling checkout the suite job pins, and is not a
# meson executable linked to the installed package. With the 1.11.0 package there is no gate
# and every commit of the test passes; against main the test before its fix died of SIGSEGV on
# a host without an enp1s0.
#
# The sibling is $LIBREAC_SRCDIR, else ../libreac. Absent and unset: SKIP (77), the rpm %check
# has none. SET but not a libreac checkout: FAIL, naming it.
#
#   tests/ifscan-against-libreac-main.sh <reac-pw source dir>
set -uo pipefail
HERE="${1:?usage: ifscan-against-libreac-main.sh SRCDIR}"
if [ -n "${LIBREAC_SRCDIR:-}" ]; then
	LR="$LIBREAC_SRCDIR"
	for f in include/reac/transport/reac_ifscan.h src transport/src; do
		[ -e "$LR/$f" ] || { echo "FAIL: LIBREAC_SRCDIR=$LR is not a libreac checkout (missing: $f)" >&2; exit 1; }
	done
else
	LR="$HERE/../libreac"
	[ -e "$LR/include/reac/transport/reac_ifscan.h" ] \
		|| { echo "SKIP: no libreac sibling at $LR (set LIBREAC_SRCDIR)"; exit 77; }
fi
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
ok=1
for f in "$LR"/src/*.c "$LR"/transport/src/*.c; do
	gcc -std=gnu11 -O1 -g -D_GNU_SOURCE -w -I"$LR/include" -I"$LR/transport/src" -c "$f" \
		-o "$D/$(basename "$(dirname "$(dirname "$f")")")_$(basename "$f" .c).o" 2>"$D/cc.err" \
		|| { echo "FAIL: libreac's $f did not compile:" >&2; head -5 "$D/cc.err" >&2; ok=0; }
done
[ "$ok" = 1 ] || exit 1
gcc -std=gnu11 -O1 -g -D_GNU_SOURCE -I"$LR/include" "$HERE/tests/test_reac_ifscan.c" "$D"/*.o \
	-lm -lpthread -o "$D/test_reac_ifscan" 2>"$D/cc.err" \
	|| { echo "FAIL: test_reac_ifscan did not compile against $LR:" >&2; head -8 "$D/cc.err" >&2; exit 1; }
echo "ifscan test against libreac $(git -C "$LR" log --oneline -1 2>/dev/null || echo "$LR")"
"$D/test_reac_ifscan"
