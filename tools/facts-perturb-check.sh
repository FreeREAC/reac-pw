#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COPY FINDER: build reac-pw against a PERTURBED protocol and run the unit suite.
#
#   tools/facts-perturb-check.sh REAC_PROTOCOL [SEED...]     (default seeds: 1 2 3)
#
# reac-protocol's `gen-facts.py --perturb SEED` writes a consistent but FICTIONAL fact set:
# every free number moved within its legal range, every derived fact recomputed, every law
# kept (docs/audits/2026-09-25-contract-copies.md, "Perturbation mode"). A reac-pw that
# reads every number from reac_facts.h builds and passes its own consistency tests against
# any seed. A literal left behind disagrees with the header it included, and the test that
# looks at it goes red. That is the whole point: the red names the copy.
#
# SOME REDS ARE NOT reac-pw's COPIES, and they are listed, one per line, with the reason, in
# tests/facts-perturb-debt.txt:
#   libreac  the test crosses into libreac, which is compiled against the REAL protocol
#            (its own hand copies are libreac's migration, not ours);
#   fixture  the test compares against captured bytes, which are the real protocol;
#   copy     a reac-pw copy not yet replaced.
# THE LIST ONLY SHRINKS. A test that goes red and is not listed FAILS this check (a new
# copy); a listed test that passes under every seed FAILS it too (remove the line); and the
# list may not grow past the `ceiling:` its header states.
#
# The verdict is the last line: `facts-perturb-check: PASS` or `facts-perturb-check: FAIL`.
# Each seed's per-test results are read from meson's own testlog.json, never from a count.
set -uo pipefail
RP="${1:?usage: $0 REAC_PROTOCOL [SEED...]}"; shift
SEEDS=("$@"); [ ${#SEEDS[@]} -gt 0 ] || SEEDS=(1 2 3)
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEBT="$HERE/tests/facts-perturb-debt.txt"
# Outside the source tree: meson refuses an absolute include directory inside it.
WORK="${FACTS_PERTURB_WORK:-${TMPDIR:-/tmp}/reac-pw-facts-perturb}"
verdict() { echo "facts-perturb-check: $1"; exit "$2"; }

[ -f "$DEBT" ] || { echo "no $DEBT"; verdict FAIL 1; }
mkdir -p "$WORK"
# CANONICAL, because meson compares paths as strings: "$HERE/../x" starts with the source
# root and is refused as an absolute include directory inside it, though it is not.
WORK=$(cd "$WORK" && pwd -P)
FAILED="$WORK/failed.txt"; : > "$FAILED"

for seed in "${SEEDS[@]}"; do
	dir="$WORK/facts-$seed" bld="$WORK/build-$seed"
	rm -rf "$dir"
	python3 "$HERE/tools/facts.py" perturb "$RP" "$seed" "$dir" \
		|| { echo "seed $seed: reac-protocol could not perturb"; verdict FAIL 1; }
	if [ -f "$bld/build.ninja" ]; then
		meson configure "$bld" -Dfacts_dir="$dir" >/dev/null || verdict FAIL 1
	else
		meson setup "$bld" "$HERE" -Dfacts_dir="$dir" >"$WORK/setup-$seed.log" 2>&1 \
			|| { grep -E 'ERROR' "$WORK/setup-$seed.log" | head -5
			     echo "seed $seed: meson setup failed"; verdict FAIL 1; }
	fi
	# A PERTURBED BUILD MUST BUILD. A literal array bound or a static_assert that only
	# holds for the real numbers is a copy too, and it cannot be listed as debt.
	meson compile -C "$bld" >"$WORK/compile-$seed.log" 2>&1 || {
		grep -E 'error' "$WORK/compile-$seed.log" | head -20
		echo "seed $seed: reac-pw does not BUILD against a perturbed protocol"
		verdict FAIL 1; }
	meson test -C "$bld" --no-rebuild --no-suite netns --no-suite load \
		>"$WORK/test-$seed.log" 2>&1
	python3 - "$bld/meson-logs/testlog.json" "$seed" >>"$FAILED" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    r = json.loads(line)
    if r["result"] not in ("OK", "SKIP", "EXPECTEDFAIL"):
        print(r["name"].split(" / ")[-1], sys.argv[2])
PY
	echo "seed $seed: $(awk -v s="$seed" '$2 == s' "$FAILED" | wc -l) test(s) red against the perturbed set"
done

python3 - "$DEBT" "$FAILED" <<'PY'
import re, sys
debt, ceiling = {}, None
for line in open(sys.argv[1]):
    m = re.match(r"#\s*ceiling:\s*(\d+)", line)
    if m:
        ceiling = int(m.group(1))
    line = line.split("#", 1)[0].strip()
    if line:
        name, _, why = line.partition(" ")
        debt[name] = why.strip()
red = {}
for line in open(sys.argv[2]):
    name, seed = line.split()
    red.setdefault(name, []).append(seed)
bad = 0
for name in sorted(set(red) - set(debt)):
    print(f"NEW COPY: {name} goes red against the perturbed protocol (seed {', '.join(red[name])}) "
          "and is not in tests/facts-perturb-debt.txt - a number it uses is spelled, not read")
    bad = 1
for name in sorted(set(debt) - set(red)):
    print(f"STALE DEBT: {name} passes under every seed - remove its line (the list only shrinks)")
    bad = 1
if ceiling is None or len(debt) > ceiling:
    print(f"DEBT CEILING: {len(debt)} entries, ceiling {ceiling} - the list may only shrink")
    bad = 1
for name in sorted(set(red) & set(debt)):
    print(f"  debt: {name} ({debt[name]})")
print(f"{len(debt)} debt entries (ceiling {ceiling}), {len(red)} red test(s) across the seeds")
sys.exit(bad)
PY
[ $? -eq 0 ] && verdict PASS 0
verdict FAIL 1
