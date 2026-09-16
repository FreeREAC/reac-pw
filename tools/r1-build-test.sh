#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Build reac-pw and run its test suite on the r1 cluster node, in the reac-build container.
#
#   tools/r1-build-test.sh [worktree] [meson-test-args...]
#
# WHY A SCRIPT AND NOT THREE COMMANDS. Two traps, both paid for on 2026-09-16.
#
# 1. THE CONTAINER HAS NO libreac. The image is Fedora + gcc/meson/pipewire-devel, and this
#    build needs pkgconfig(libreac) and pkgconfig(libreac-transport). Rather than clone and
#    build libreac there -- a second libreac, which is the version-skew bug waiting to
#    happen -- this stages the DESK'S OWN INSTALLED libreac (headers, .so, .pc with the
#    prefix rewritten) and hands it over as a second tree. What runs on r1 is then the same
#    library the desk links against, by construction.
#
# 2. A STALE BUILD MADE A GREEN SUITE READ RED. `r1-c-run.sh` excludes `_build` from the
#    rsync, and rsync PRESERVES SOURCE MTIMES -- so a source restored to an older mtime
#    than the object built from a newer one leaves ninja with nothing to do, and the suite
#    runs the PREVIOUS tree. Measured: two unit tests reported the exact failures of a
#    sabotage that had already been reverted. So every run TOUCHES the sources before it
#    compiles; a full rebuild on 24 cores is cheap and a suite that tests a tree nobody has
#    is not.
set -o pipefail
TREE="${1:-$(cd "$(dirname "$0")/.." && pwd)}"; shift 2>/dev/null
RUNNER="$HOME/.claude/openmixer/r1-c-run.sh"
[ -x "$RUNNER" ] || { echo "r1-build-test: no $RUNNER" >&2; exit 2; }

PREFIX=$(mktemp -d)
trap 'rm -rf "$PREFIX"' EXIT
mkdir -p "$PREFIX/libreac-prefix/include" "$PREFIX/libreac-prefix/lib64/pkgconfig" || exit 2
cp -a /usr/include/reac "$PREFIX/libreac-prefix/include/" || {
	echo "r1-build-test: no /usr/include/reac -- install libreac-devel on this desk" >&2; exit 2; }
cp -a /usr/lib64/libreac.so* /usr/lib64/libreac-transport.so* "$PREFIX/libreac-prefix/lib64/" || exit 2
for f in libreac libreac-transport; do
	sed 's|^prefix=/usr|prefix=/w/libreac-prefix|' "/usr/lib64/pkgconfig/$f.pc" \
		> "$PREFIX/libreac-prefix/lib64/pkgconfig/$f.pc" || exit 2
done

NAME=$(basename "$TREE")
# ONE ARGUMENT, BUILT BY A QUOTED HEREDOC, AND NO SINGLE QUOTE ANYWHERE IN IT: r1-c-run.sh
# wraps the command in single quotes for the remote shell, and a double quote here would
# close this script own quoting. Both were hit writing this.
CMD=$(cat <<EOS
export PKG_CONFIG_PATH=/w/libreac-prefix/lib64/pkgconfig LD_LIBRARY_PATH=/w/libreac-prefix/lib64
cd /w/$NAME || exit 2
meson setup _build >/tmp/setup.log 2>&1 || meson setup --reconfigure _build >/tmp/setup.log 2>&1 || { tail -20 /tmp/setup.log; exit 2; }
find src tests tools meson.build -type f -exec touch {} + || exit 2
meson compile -C _build >/tmp/compile.log 2>&1 || { grep -E "error:|FAILED" /tmp/compile.log | head -20; exit 1; }
meson test -C _build --print-errorlogs $* 2>&1 | tail -40
EOS
)
nice -n 19 "$RUNNER" "$NAME" "$PREFIX/libreac-prefix" "$TREE" -- "$CMD"
