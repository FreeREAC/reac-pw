#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Build reac-pw and run its suite on r1 against a LIBREAC WORKTREE, not the desk's
# installed RPM.
#
#   tools/r1-build-test-libreac.sh <libreac-worktree> [reac-pw-worktree] [meson-test-args...]
#
# tools/r1-build-test.sh stages /usr/include/reac and /usr/lib64/libreac.so, which is the
# right thing when the library is not what changed. A lane that moves BOTH repos cannot
# use it: it would compile the new daemon against the old library and report green for a
# pair that does not exist. This builds the library from the worktree first, in the same
# container, and hands reac-pw a pkg-config prefix pointing at it.
#
# STATIC ON PURPOSE. The staged prefix carries libreac.a / libreac-transport.a, so
# `-lreac` resolves to the archive and nothing has to be installed or LD_LIBRARY_PATHed.
# The .pc carries -lm: libreac's encoder calls lrintf, and an archive resolves nothing
# for its consumer (the shared library does, which is why this only bites here).
#
# THE BUILD DIR IS WIPED, NOT REUSED. `_build` survives on r1 between runs (r1-c-run.sh
# excludes it from the rsync, so --delete leaves it), and meson CACHES the pkg-config
# answer — so a rerun after fixing the prefix re-linked against the old flags and failed
# identically. Measured 2026-09-17.
set -o pipefail
LIBREAC="${1:?usage: r1-build-test-libreac.sh <libreac-worktree> [reac-pw-worktree] [args]}"; shift
TREE="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[ -d "$1" ] && shift
RUNNER="$HOME/.claude/openmixer/r1-c-run.sh"
[ -x "$RUNNER" ] || { echo "r1-build-test-libreac: no $RUNNER" >&2; exit 2; }
[ -f "$LIBREAC/include/reac/reac.h" ] || { echo "r1-build-test-libreac: $LIBREAC is not a libreac tree" >&2; exit 2; }

L=$(basename "$LIBREAC"); P=$(basename "$TREE")
# One argument, quoted heredoc, no single quotes: r1-c-run.sh wraps this in single quotes.
CMD=$(cat <<EOS2
set -e
cd /w/$L
make -s all >/dev/null
make -s REACPW_INCLUDE=/w/$P/src transport >/dev/null
mkdir -p /w/pfx/include/reac/transport /w/pfx/lib64/pkgconfig
cp include/reac/*.h /w/pfx/include/reac/
cp include/reac/transport/*.h /w/pfx/include/reac/transport/
cp libreac.a libreac-transport.a /w/pfx/lib64/
V=\$(sed -n 4p packaging/libreac.spec | tr -dc 0-9.)
for f in libreac libreac-transport; do
  N=reac; [ \$f = libreac-transport ] && N=reac-transport
  printf 'prefix=/w/pfx\nexec_prefix=\${prefix}\nlibdir=\${exec_prefix}/lib64\nincludedir=\${prefix}/include\n\nName: %s\nDescription: worktree build\nVersion: %s\nLibs: -L\${libdir} -l%s -lm\nCflags: -I\${includedir}\n' \$f \$V \$N > /w/pfx/lib64/pkgconfig/\$f.pc
done
export PKG_CONFIG_PATH=/w/pfx/lib64/pkgconfig
cd /w/$P
{ [ -d _build ] && meson setup --wipe _build >/tmp/s.log 2>&1 ; } || meson setup _build >/tmp/s.log 2>&1 || { tail -20 /tmp/s.log; exit 2; }
find src tests tools meson.build -type f -exec touch {} +
meson compile -C _build >/tmp/c.log 2>&1 || { grep -E "undefined|error:|FAILED" /tmp/c.log | grep -v Wformat | head -25; exit 1; }
meson test -C _build --print-errorlogs $* 2>&1 | tail -40
EOS2
)
nice -n 19 "$RUNNER" "$P" "$LIBREAC" "$TREE" -- "$CMD"
