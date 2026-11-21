#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# EVERY NAME THE DAEMON READS THROUGH reac_conf_lookup IS A KNOB: in g_reac_knobs (src/reac_knobs.c,
# so the startup announce names it) and in docs/ENV-KNOBS.md (so an operator can find it).
# REACPW_LAUNCH_STAMPS was read at src/reac_sink_node.c with neither. A SWEEP THAT CANNOT SEE
# PROVES IT CAN: the extractor runs first on a planted two-line call.
#
#   tests/every-conf-lookup-name-is-a-knob.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
names() { # <file...>: the literal names passed as the first argument of reac_conf_lookup, calls may wrap
	cat "$@" | tr '\n' ' ' | grep -oE 'reac_conf_lookup\( *"[A-Z][A-Z_0-9]*"' | sed -E 's/.*"([^"]*)"/\1/' | sort -u
}
t=$(mktemp); trap 'rm -f "$t"' EXIT
printf 'x = reac_conf_lookup(\n    "REACPW_PLANTED",\n  a);\n' >"$t"
[ "$(names "$t")" = REACPW_PLANTED ] \
	|| { echo "FAIL: self-check: the extractor cannot read a wrapped reac_conf_lookup call" >&2; exit 1; }
fail=0
for k in $(names "$ROOT"/src/*.c); do
	# a RETIRED key is read only to be named as ignored: the doc says so and it has no table row
	grep -F "$k" "$ROOT/docs/ENV-KNOBS.md" | grep -q 'RETIRED' \
		|| grep -qF "\"$k\"" "$ROOT/src/reac_knobs.c" || { echo "FAIL: $k is read through reac_conf_lookup but is not in g_reac_knobs (src/reac_knobs.c)" >&2; fail=1; }
	grep -qF "$k" "$ROOT/docs/ENV-KNOBS.md" || { echo "FAIL: $k is read through reac_conf_lookup but is not in docs/ENV-KNOBS.md" >&2; fail=1; }
done
[ "$fail" = 0 ] && echo "OK: every reac_conf_lookup name is in g_reac_knobs and docs/ENV-KNOBS.md"
exit "$fail"
