#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble the reac-pw source tarball for rpmbuild. The RPM builds against the
# SYSTEM libreac (BuildRequires: pkgconfig(libreac); the spec passes
# --wrap-mode=nofallback so the subproject fallback can never mask a missing
# libreac-devel) -- nothing is vendored any more. The meson wrap in
# subprojects/ stays in-repo for dev/git builds without libreac-devel, but is
# deliberately NOT shipped in the tarball. Writes reac-pw-<version>.tar.gz to
# the repo root.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Version single-source: meson.build's project version (the spec's fallback tracks
# it; a release build overrides both via $1 here + --define version_override there).
MESON_V=$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)
V="${1:-$MESON_V}"
[ -n "$V" ] || { echo "could not read version from $ROOT/meson.build"; exit 1; }

T=$(mktemp -d); D="$T/reac-pw-$V"
mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' "$ROOT/src" "$ROOT/tests" "$ROOT/tools" \
      "$ROOT/meson.build" "$ROOT/meson_options.txt" "$ROOT/LICENSE" "$ROOT/README.md" \
      "$ROOT/packaging" "$ROOT/docs" "$D/"
tar -czf "$ROOT/reac-pw-$V.tar.gz" -C "$T" "reac-pw-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-pw-$V.tar.gz"
