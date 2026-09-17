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
# The work itself is tools/r1-inner-build-libreac.sh, which runs in the container: this
# half only names the trees. The split is not cosmetic — r1-c-run.sh single-quotes its
# command for the remote shell, so a quote in the staging printf ends it.
set -o pipefail
LIBREAC="${1:?usage: r1-build-test-libreac.sh <libreac-worktree> [reac-pw-worktree] [args]}"; shift
TREE="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[ -d "$1" ] && shift
RUNNER="$HOME/.claude/openmixer/r1-c-run.sh"
[ -x "$RUNNER" ] || { echo "r1-build-test-libreac: no $RUNNER" >&2; exit 2; }
[ -f "$LIBREAC/include/reac/reac.h" ] || { echo "r1-build-test-libreac: $LIBREAC is not a libreac tree" >&2; exit 2; }

L=$(basename "$LIBREAC"); P=$(basename "$TREE")
nice -n 19 "$RUNNER" "$P" "$LIBREAC" "$TREE" -- "bash /w/$P/tools/r1-inner-build-libreac.sh $L $P $*"
