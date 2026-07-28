#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a self-contained reac-pw source tarball for rpmbuild: vendors libreac
# (so the meson wrap needs no network) and the reac-aes67 decode core (so there
# is no sibling-checkout dependency). Writes reac-pw-<version>.tar.gz to the repo
# root. Override the core location with REAC_AES67=/path/to/reac-aes67 checkout.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Version single-source: meson.build's project version (the spec's fallback tracks
# it; a release build overrides both via $1 here + --define version_override there).
MESON_V=$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)
V="${1:-$MESON_V}"
[ -n "$V" ] || { echo "could not read version from $ROOT/meson.build"; exit 1; }
AES="${REAC_AES67:-$ROOT/../reac-aes67-pub}"
[ -f "$AES/src/reac_decode.c" ] || { echo "reac-aes67 core not found at $AES"; exit 1; }

T=$(mktemp -d); D="$T/reac-pw-$V"
mkdir -p "$D"
rsync -a --exclude '.git' --exclude 'build' "$ROOT/src" "$ROOT/tests" \
      "$ROOT/meson.build" "$ROOT/meson_options.txt" "$ROOT/LICENSE" "$ROOT/README.md" \
      "$ROOT/packaging" "$D/"
# vendor libreac (wrap + packagefiles + the checkout, minus its git/build)
mkdir -p "$D/subprojects"
cp "$ROOT/subprojects/libreac.wrap" "$D/subprojects/"
rsync -a "$ROOT/subprojects/packagefiles" "$D/subprojects/"
rsync -a --exclude '.git' --exclude 'build' "$ROOT/subprojects/libreac" "$D/subprojects/"
# vendor the reac-aes67 decode/capture/pcap core
mkdir -p "$D/third_party/reac-aes67-core/src"
for fcore in reac_decode reac_capture pcap_source; do
	cp "$AES/src/$fcore.c" "$AES/src/$fcore.h" "$D/third_party/reac-aes67-core/src/"
done
tar -czf "$ROOT/reac-pw-$V.tar.gz" -C "$T" "reac-pw-$V"
rm -rf "$T"
echo "wrote $ROOT/reac-pw-$V.tar.gz"
