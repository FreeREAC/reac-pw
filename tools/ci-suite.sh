#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE WHOLE SUITE, WHERE IT CAN FAIL. The body of .github/workflows/test.yml (audit
# 2026-09-24, H3), kept here so the same thing runs on a desk:
#
#   tools/ci-suite.sh <libreac-checkout> [builddir]
#
# Builds reac-pw against the libreac checkout (tools/build-with-libreac.sh), builds
# libreac's fake_box, then runs the suite the way the RPM %check does: everything but
# `netns` and `load` in parallel, then `netns` one at a time.
#
# AND THEN IT REFUSES A NAMESPACE SUITE THAT SKIPPED. The netns tests skip, correctly, on a
# machine that cannot run them (no user namespaces, no 8021q, no sch_etf). That is the right
# answer on a builder and the wrong one here: this runner is provisioned to run every one of
# them, so a SKIP here means a namespace test did not run, and that is how the 1.0.26
# release job read `Ok: 0 Fail: 0 Skipped: 30` as a pass. The verdict below is read from
# meson's own per-test result, never from a count.
set -uo pipefail
LIBREAC="${1:?usage: $0 <libreac-checkout> [builddir]}"
BUILD="${2:-build-ci}"
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIBREAC=$(cd "$LIBREAC" && pwd)

"$HERE/tools/build-with-libreac.sh" "$LIBREAC" "$BUILD" || exit 1
make -C "$LIBREAC" fake_box || exit 1
meson configure "$HERE/$BUILD" -Dfake_box="$LIBREAC/fake_box" \
	-Dlibreac_transport_srcdir="$LIBREAC/transport/src" || exit 1
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
exit $rc
