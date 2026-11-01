#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# What locking the slave side's receive gate to its desk changes over recorded sessions.
#
#   tools/rx-bind-replay.sh <libreac-checkout> <captures-dir> [--sabotage]
#
# Builds tools/rx_bind_replay.c against the libreac checkout (libreac.a, libreac-transport.a,
# built here if absent), then
#   1. replays every *.pcap under the captures directory through reac_rx unlocked and locked
#      to the peer the hunt names, and refuses any difference tools/rx_bind_replay.c cannot
#      explain from the frame itself;
#   2. forges a second host's stream into a real capture and requires the unlocked gate to
#      decode it and the locked one not to (the control: the locked gate keeps the desk).
# --sabotage rebuilds with the lock call removed from src/reac_rx_bind.c and requires step 2
# to go red, so the arm is known to see what it claims to.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
LR=$(cd "${1:?libreac checkout}" && pwd)
CAPS=${2:?captures dir}
SAB=${3:-}
OUT=${TMPDIR:-/tmp}/rx-bind-replay
mkdir -p "$OUT"
make -s -C "$LR" libreac.a libreac-transport.a REACPW_INCLUDE="$HERE/src" >/dev/null
build() {
	${CC:-cc} -O2 -std=c11 -Wall -Wextra -I"$LR/include" \
	    "$HERE/tools/rx_bind_replay.c" "$1" "$LR/libreac-transport.a" "$LR/libreac.a" \
	    -lm -lpthread -Wl,--wrap=pcap_source_next -o "$2"
}
build "$HERE/src/reac_rx_bind.c" "$OUT/rx_bind_replay"
if [ "$SAB" = --sabotage ]; then
	sed 's|\treac_rx_peer_reset(rx, mac, REAC_RX_BIND_SESSION);|\t(void)rx;|' \
	    "$HERE/src/reac_rx_bind.c" > "$OUT/sab_rx_bind.c"
	sed -i 's|#include "reac_rx_bind.h"|#include "'"$HERE"'/src/reac_rx_bind.h"|' "$OUT/sab_rx_bind.c"
	build "$OUT/sab_rx_bind.c" "$OUT/rx_bind_replay_sab"
	pc=$(find "$CAPS" -name '*.pcap' | sort | head -400)
	for p in $pc; do
		if "$OUT/rx_bind_replay_sab" attack "$p" >"$OUT/sab.txt" 2>&1; then
			echo "SABOTAGE NOT CAUGHT: attack arm stayed green with the lock removed ($p)"; exit 1
		fi
		case $(cat "$OUT/sab.txt") in *"names no desk"*|*"only "*) continue ;; esac
		echo "sabotage caught on $p:"; cat "$OUT/sab.txt"; exit 0
	done
	echo "sabotage: no capture to attack"; exit 1
fi
rc=0
find "$CAPS" -name '*.pcap' | sort | while read -r p; do echo "$p"; done > "$OUT/list.txt"
# shellcheck disable=SC2046
"$OUT/rx_bind_replay" corpus $(cat "$OUT/list.txt") > "$OUT/corpus.txt" || rc=1
cat "$OUT/corpus.txt"
echo "== summary"
echo "captures: $(wc -l < "$OUT/list.txt")"
echo "unbound (no desk or box to bind, nothing can differ): $(grep -c ' unbound ' "$OUT/corpus.txt" || true)"
echo "bound: $(grep -c ' bound=' "$OUT/corpus.txt" || true)"
echo "with a difference: $(grep ' bound=' "$OUT/corpus.txt" | grep -vc 'foreign_refused=0 shadow_recovered=0' || true)"
echo "unexplained frames: $(grep -c UNEXPLAINED "$OUT/corpus.txt" || true)"
echo "== attack"
att=0
for p in $(cat "$OUT/list.txt"); do
	if "$OUT/rx_bind_replay" attack "$p" > "$OUT/att.txt" 2>"$OUT/att.err"; then
		cat "$OUT/att.txt"; att=1; break
	fi
	case $(cat "$OUT/att.err") in *"names no desk"*|*"only "*) continue ;; esac
	cat "$OUT/att.txt" "$OUT/att.err"; echo "ATTACK ARM RED on $p"; exit 1
done
[ $att = 1 ] || { echo "attack arm found no capture with a desk to forge against"; exit 1; }
exit $rc
