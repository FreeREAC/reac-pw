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
rpmbuild -ba --define "_topdir $TOP" "$SPEC"
