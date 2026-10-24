#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE WHOLE SUITE, WHERE IT CAN FAIL. The body of .github/workflows/test.yml (audit
# 2026-09-24, H3), kept here so the same thing runs on a desk:
#
#   tools/ci-suite.sh [builddir] [reac-protocol-checkout]
#
# Builds reac-pw against the INSTALLED libreac packages and runs the suite the way the RPM %check
# does: everything but `netns` and `load` in parallel, then `netns` one at a time. fake_box, the far
# end several tests drive, is the one libreac-tests installs (FAKE_BOX overrides the path). The
# packages come from the channel by `dnf builddep packaging/reac-pw.spec` /
# `apt-get build-dep ./`, which hold the floor and the ceiling; this script finds what is installed.
#
# AND THEN IT REFUSES A NAMESPACE SUITE THAT SKIPPED. The netns tests skip, correctly, on a
# machine that cannot run them (no user namespaces, no 8021q, no sch_etf). That is the right
# answer on a builder and the wrong one here: this runner is provisioned to run every one of
# them, so a SKIP here means a namespace test did not run, and that is how the 1.0.26
# release job read `Ok: 0 Fail: 0 Skipped: 30` as a pass. The verdict below is read from
# meson's own per-test result, never from a count.
set -uo pipefail
BUILD="${1:-build-ci}"
RP="${2:-}"
HERE=$(cd "$(dirname "$0")/.." && pwd)
FAKE_BOX="${FAKE_BOX:-/usr/libexec/libreac/tests/fake_box}"

# THE FLOOR IS THE PACKAGES', AND THIS NAMES WHAT IS INSTALLED. A missing or too old libreac fails
# meson's own version check below; a missing fake_box would only skip its tests, so it is refused here.
[ -x "$FAKE_BOX" ] || { echo "ci-suite: no fake_box at $FAKE_BOX (the libreac-tests package)" >&2; exit 2; }
echo "== libreac $(pkg-config --modversion libreac), libreac-transport $(pkg-config --modversion libreac-transport), fake_box $FAKE_BOX"
if [ -f "$HERE/$BUILD/build.ninja" ]; then
	meson configure "$HERE/$BUILD" -Dfake_box="$FAKE_BOX" || exit 1
else
	meson setup "$HERE/$BUILD" "$HERE" -Dfake_box="$FAKE_BOX" --wrap-mode=nofallback || exit 1
fi
meson compile -C "$HERE/$BUILD" || exit 1

rc=0
meson test -C "$HERE/$BUILD" --no-suite netns --no-suite load --print-errorlogs || rc=1
cp "$HERE/$BUILD/meson-logs/testlog.json" "$HERE/$BUILD/meson-logs/testlog-unit.json"
meson test -C "$HERE/$BUILD" --no-suite load --suite netns --num-processes 1 --print-errorlogs || rc=1

# EVERY NAMESPACE TEST RAN, OR THIS RUN IS RED. A netns test reporting anything but OK is
# listed with its own last lines, so the reason is on the page.
python3 - "$HERE/$BUILD/meson-logs/testlog.json" <<'PY' || rc=1
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
if not rows:
    print("FAIL: the netns run recorded no tests at all"); sys.exit(1)
bad = [r for r in rows if r["result"] != "OK"]
for r in bad:
    print(f"NOT OK: {r['name']}: {r['result']}")
    for line in (r.get("stdout") or "").strip().splitlines()[-4:]:
        print(f"    {line}")
if bad:
    print(f"FAIL: {len(bad)} of {len(rows)} namespace tests did not pass on a runner provisioned for all of them")
    sys.exit(1)
print(f"OK: all {len(rows)} namespace tests ran and passed")
PY

# EVERY PROTOCOL NUMBER FROM ITS ONE DECLARATION. With a reac-protocol checkout, build
# against three perturbed fact sets and run the unit suite: a reac-pw copy of a number
# goes red and is named, and the known reds are the shrink-only tests/facts-perturb-debt.txt.
if [ -n "$RP" ]; then
	FACTS_PERTURB_WORK="$HERE/../reac-pw-facts-perturb" \
		"$HERE/tools/facts-perturb-check.sh" "$RP" 1 2 3 || rc=1
fi
exit $rc
