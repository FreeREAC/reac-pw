#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Build reac-pw against a SIBLING libreac CHECKOUT, with no RPM and nothing installed on
# the machine. The documented path for a lane whose libreac half is unreleased, and for the
# r1 build container, where the only libreac that exists is the one rsynced in beside us.
#
#   tools/build-with-libreac.sh ../libreac-wt-<lane> [builddir] [-- meson-test-args...]
#
# WHY THIS EXISTS. reac-pw's meson requires pkgconfig(libreac) AND
# pkgconfig(libreac-transport) at a hand-raised version floor, and libreac ships a hand
# Makefile whose .pc files are written by its RPM spec — so a checkout pair alone cannot be
# configured, and every lane re-derived the same twenty lines by hand. The two templates
# below are copied from libreac's own packaging/*.spec and the VERSION is read from those
# specs, so a floor that moves in libreac moves here with it instead of being re-typed.
#
# STATIC, ON PURPOSE. The prefix holds the .a files, so the binary it produces needs no
# LD_LIBRARY_PATH and cannot silently pick up an installed libreac of a different version —
# the exact confusion this script is meant to end. `pkg-config --modversion libreac` on a
# desk that has the RPMs still answers the RPM's version; the PKG_CONFIG_PATH below puts
# this prefix FIRST so the checkout wins.
set -o pipefail
LIBREAC="${1:?usage: $0 <libreac-checkout> [builddir] [-- meson test args]}"; shift
BUILD="build"
case "${1:-}" in --) ;; "") ;; *) BUILD="$1"; shift ;; esac
[ "${1:-}" = "--" ] && shift
[ -f "$LIBREAC/Makefile" ] || { echo "$0: $LIBREAC is not a libreac checkout" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")/.." && pwd)
LIBREAC=$(cd "$LIBREAC" && pwd)
PREFIX="$HERE/$BUILD/_libreac-prefix"

ver() { sed -n 's/^Version: *//p' "$LIBREAC/packaging/$1.spec" | head -1; }
V=$(ver libreac); VT=$(ver libreac-transport)
[ -n "$V" ] && [ -n "$VT" ] || { echo "$0: no Version in libreac's specs" >&2; exit 2; }

# REACPW_INCLUDE IS REQUIRED, NOT OPTIONAL, FOR THE TRANSPORT HALF. Two transport headers
# still #include reac-pw's own reac_rate_cfg.h / reac_role_cfg.h for their pure
# declarations (the transport-library spec names that seam), and libreac's Makefile leaves
# the path unset so those two objects fail LOUDLY instead of being skipped. We are the
# reac-pw checkout it wants, so we point it at ourselves.
echo "== libreac $V / libreac-transport $VT from $LIBREAC"
make -C "$LIBREAC" -j"$(nproc)" REACPW_INCLUDE="$HERE/src" all transport || exit 1

rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include/reac/transport"
cp "$LIBREAC/libreac.a" "$PREFIX/lib/" || exit 1
cp "$LIBREAC/libreac-transport.a" "$PREFIX/lib/" || exit 1
cp "$LIBREAC"/include/reac/*.h "$PREFIX/include/reac/" || exit 1
cp "$LIBREAC"/include/reac/transport/*.h "$PREFIX/include/reac/transport/" || exit 1

cat > "$PREFIX/lib/pkgconfig/libreac.pc" <<PC
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libreac
Description: Roland REAC wire-format core (checkout build)
Version: $V
Libs: -L\${libdir} -lreac -lm
Cflags: -I\${includedir}
PC

cat > "$PREFIX/lib/pkgconfig/libreac-transport.pc" <<PC
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libreac-transport
Description: The REAC transport layer (checkout build)
Version: $VT
Requires: libreac >= $V
Libs: -L\${libdir} -lreac-transport -lpthread -lm
Cflags: -I\${includedir}
PC

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# THE ARCHIVE ORDER IS LOAD-BEARING WITH STATIC LIBS: transport calls into libreac, so
# -lreac must follow -lreac-transport. pkg-config emits Requires: after Libs:, which is
# exactly that order, and this line exists to say so rather than to be rediscovered.
# A BUILD DIR CARRIES THE ABSOLUTE PATHS OF THE TREE THAT CONFIGURED IT, so one rsynced
# to another machine (or copied between worktrees) reconfigures into directories that are
# not there — met on r1, where `meson setup --reconfigure` died inside tempfile.mkdtemp on
# the desk's own path. A reconfigure that fails is therefore not an error to report: it is
# a build dir that belongs to somewhere else, and the answer is to throw it away.
if [ -f "$HERE/$BUILD/build.ninja" ] && meson setup --reconfigure "$HERE/$BUILD" "$HERE" >/dev/null 2>&1; then
	:
else
	rm -rf "$HERE/$BUILD"/meson-* "$HERE/$BUILD"/build.ninja "$HERE/$BUILD"/.ninja*
	meson setup "$HERE/$BUILD" "$HERE" || exit 1
fi
meson compile -C "$HERE/$BUILD" || exit 1
echo "== built $HERE/$BUILD/reac-pw"
[ $# -gt 0 ] && exec meson test -C "$HERE/$BUILD" "$@"
exit 0
