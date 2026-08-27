#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build this package's RPMs.
#
# _topdir is forced to the PHYSICAL path of ~/rpmbuild. Where ~/rpmbuild is a
# symlink, meson/ninja record the resolved physical directory in DW_AT_comp_dir
# while rpm's debugedit looks under the logical one, so it finds no sources and
# the build dies at the very end with:
#
#     error: Empty %files file .../debugsourcefiles.list
#
# after %build and %check have both passed -- which reads like a packaging bug
# and is not one.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SPEC=$(ls "$ROOT"/packaging/*.spec | head -1)
TOP=$(readlink -f "${RPM_TOPDIR:-$HOME/rpmbuild}")
sh "$ROOT/packaging/make-tarball.sh" "$@"
mkdir -p "$TOP/SOURCES"
cp "$ROOT"/*.tar.gz "$TOP/SOURCES/"
# Single-source the version from meson.build (or $1), and PASS it to rpmbuild — without
# this the spec fell back to its 0.3.0 default whatever meson said (found 2026-08-27).
V="${1:-$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)}"
rpmbuild -ba --define "_topdir $TOP" --define "version_override $V" "$SPEC"
