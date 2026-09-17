#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE CONTAINER HALF of tools/r1-build-test-libreac.sh. Runs inside reac-build:1 on r1,
# where both worktrees are mounted under /w. Not for the desk.
#
#   r1-inner-build-libreac.sh <libreac-dirname> <reac-pw-dirname> [meson-test-args...]
#
# IT IS A FILE AND NOT A -c STRING because r1-c-run.sh wraps its command in SINGLE
# QUOTES for the remote shell, so a single quote anywhere in that string ends it — and
# staging a pkg-config file needs printf, which needs quotes. The first cut of this tool
# was inline and died on `syntax error: unexpected end of file from for`.
set -e
L="${1:?libreac dirname under /w}"; P="${2:?reac-pw dirname under /w}"; shift 2
cd "/w/$L"
make -s all >/dev/null
make -s REACPW_INCLUDE="/w/$P/src" transport >/dev/null
mkdir -p /w/pfx/include/reac/transport /w/pfx/lib64/pkgconfig
cp include/reac/*.h /w/pfx/include/reac/
cp include/reac/transport/*.h /w/pfx/include/reac/transport/
cp libreac.a libreac-transport.a /w/pfx/lib64/
V=$(sed -n 4p packaging/libreac.spec | tr -dc 0-9.)
for f in libreac libreac-transport; do
	N=reac; [ "$f" = libreac-transport ] && N=reac-transport
	# STATIC, so -lreac resolves to the archive and nothing is installed; -lm because
	# libreac's encoder calls lrintf and an archive resolves nothing for its consumer.
	cat > "/w/pfx/lib64/pkgconfig/$f.pc" <<PC
prefix=/w/pfx
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib64
includedir=\${prefix}/include

Name: $f
Description: worktree build
Version: $V
Libs: -L\${libdir} -l$N -lm
Cflags: -I\${includedir}
PC
done
export PKG_CONFIG_PATH=/w/pfx/lib64/pkgconfig
cd "/w/$P"
# THE BUILD DIR IS WIPED, NOT REUSED: `_build` survives between runs (r1-c-run.sh excludes
# it from the rsync, so --delete leaves it) and meson CACHES the pkg-config answer — a
# rerun after fixing the prefix re-linked with the old flags and failed identically.
if [ -d _build ]; then meson setup --wipe _build >/tmp/s.log 2>&1 || { tail -20 /tmp/s.log; exit 2; }
else meson setup _build >/tmp/s.log 2>&1 || { tail -20 /tmp/s.log; exit 2; }; fi
# rsync PRESERVES SOURCE MTIMES, so a restored source older than its object leaves ninja
# nothing to do and the suite runs the PREVIOUS tree (tools/r1-build-test.sh, 2026-09-16).
find src tests tools meson.build -type f -exec touch {} +
meson compile -C _build >/tmp/c.log 2>&1 || { grep -E 'undefined|error:|FAILED' /tmp/c.log | grep -v Wformat | head -25; exit 1; }
meson test -C _build --print-errorlogs "$@" 2>&1 | tail -40
