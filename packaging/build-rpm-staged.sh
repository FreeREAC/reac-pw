#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build reac-pw's RPM against a libreac/libreac-transport that is NOT installed on the
# host -- the soname-bump case build-rpm.sh cannot cover.
#
# Why this script exists: reac-pw.spec's BuildRequires floor moves in lockstep with
# libreac's soname (currently >= 1.1.0, libreac.so.2 -> .so.3). dnf refuses to install
# the new libreac-devel while the installed reac-pw still links the old soname -- it
# would break the installed package in the same transaction -- so the ordinary
# BuildRequires gate can never be satisfied until reac-pw itself is rebuilt. That is
# exactly the chicken-and-egg libreac's OWN build-rpm.sh already solves for its
# transport package (rpm2cpio the sibling package's fresh RPMs into a scratch prefix,
# point pkg-config at the stage, build with --nodeps). This script is that same
# mechanism, one level up: it stages libreac's *_TARGET_* RPMs (already built,
# handed in via $1) instead of building libreac itself first.
#
# Usage:
#   packaging/build-rpm-staged.sh <rpm-dir> [version]
#
# <rpm-dir> must contain libreac-<V>, libreac-devel-<V>, libreac-transport-<V> and
# libreac-transport-devel-<V> RPMs for the SAME version (V is taken from the newest
# libreac-devel-*.rpm found there unless overridden). [version] is reac-pw's own
# version, same meaning as build-rpm.sh's $1 (defaults to meson.build).
#
# --nodeps only bypasses rpmbuild's BuildRequires gate; it does not touch the built
# package's own Requires/autoreq (reac-pw.spec still declares libreac >= 1.1.0
# correctly at install time, on a host that CAN then take the transaction).
#
# %check runs `meson test`, which spawns binaries dynamically linked against
# libreac.so.3 / libreac-transport.so.4 -- neither is on the host's ld.so path (they
# are staged only), so LD_LIBRARY_PATH must point at the stage for the test run, not
# only PKG_CONFIG_PATH for the meson setup. Both are exported before rpmbuild so
# rpmbuild's %build/%check subshells inherit them.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RPM_DIR=${1:?"usage: build-rpm-staged.sh <rpm-dir-with-libreac-rpms> [version]"}
TOP=$(readlink -f "${RPM_TOPDIR:-$HOME/rpmbuild}")

# Version of the staged libreac: newest libreac-devel-*.rpm in RPM_DIR unless RPM_V is set.
RPM_V=${RPM_V:-$(ls "$RPM_DIR"/libreac-devel-*.rpm 2>/dev/null \
  | sed -n 's/.*libreac-devel-\([0-9][^-]*\)-.*/\1/p' | sort -V | tail -1)}
[ -n "$RPM_V" ] || { echo "build-rpm-staged.sh: no libreac-devel-*.rpm found in $RPM_DIR"; exit 1; }

STAGE="$TOP/stage-reac-pw-libreac-$RPM_V"
rm -rf "$STAGE"; mkdir -p "$STAGE"
# Each package's exact NAME-VERSION prefix, so libreac-$RPM_V does not also match
# libreac-devel-$RPM_V or libreac-transport-$RPM_V (ls globs are not name-boundary-aware).
for PKG in "libreac-$RPM_V" "libreac-devel-$RPM_V" "libreac-transport-$RPM_V" "libreac-transport-devel-$RPM_V"; do
	RPM=$(ls "$RPM_DIR/$PKG"-*.fc*.x86_64.rpm 2>/dev/null | head -1)
	[ -n "$RPM" ] || { echo "build-rpm-staged.sh: missing $PKG-*.rpm in $RPM_DIR"; exit 1; }
	( cd "$STAGE" && rpm2cpio "$RPM" | cpio -idm --quiet )
done

# Point pkg-config's prefix at the stage so meson resolves headers/libs from there.
for PC in "$STAGE"/usr/lib*/pkgconfig/libreac.pc "$STAGE"/usr/lib*/pkgconfig/libreac-transport.pc; do
	[ -f "$PC" ] || { echo "build-rpm-staged.sh: expected pkg-config file missing: $PC"; exit 1; }
	sed -i "s|^prefix=.*|prefix=$STAGE/usr|" "$PC"
done
PKG_CONFIG_PATH=$(dirname "$(find "$STAGE" -name libreac.pc | head -1)")
LIBDIR=$(dirname "$(find "$STAGE" -name 'libreac.so' | head -1)")
export PKG_CONFIG_PATH LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

sh "$ROOT/packaging/make-tarball.sh" "${2:-}"
mkdir -p "$TOP/SOURCES"
cp "$ROOT"/*.tar.gz "$TOP/SOURCES/"
V="${2:-$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)}"
rpmbuild -ba --nodeps --define "_topdir $TOP" --define "version_override $V" \
	"$ROOT/packaging/reac-pw.spec"
