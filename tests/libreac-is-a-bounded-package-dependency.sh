#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# NO PINS (FreeREAC/reac-pw#23): libreac is declared in the packaging and nowhere else.
#
# libreac, libreac-transport and libreac-tests (fake_box) are declared in packaging/reac-pw.spec
# and debian/control as a floor AND a ceiling (the next minor), installed from the FreeMixer
# channel by `dnf builddep` / `apt-get build-dep`, and found by pkg-config. This fails when
#   - a package the build or the tests need has a floor but no ceiling (or no floor), in the
#     spec's BuildRequires, in its Requires, or in debian/control's Build-Depends;
#   - a tracked file clones, fetches or pins a libreac source: LIBREAC_REF, a `git clone` of
#     FreeREAC/libreac, the subprojects/libreac.wrap fallback, tools/build-with-libreac.sh.
#
# A SWEEP THAT CANNOT SEE PROVES IT CAN: bounded() runs first on a declaration with no ceiling
# and must call it unbounded, and the pin pattern must match a planted line.
#
#   tests/libreac-is-a-bounded-package-dependency.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SELF=$(basename "$0")
fail=0
say() { echo "FAIL: $*" >&2; fail=1; }

# decl <text> <regex>: the lines of <text> that match
decl() { awk -v re="$2" '$0 ~ re' <<<"$1"; }

# bounded <text> <regex-for-the-name> <floor-op> <ceiling-op>
bounded() {
	grep -Eq "$2 *[)]? *$3 " <<<"$1" && grep -Eq "$2 *[)]? *$4 " <<<"$1"
}
PIN='LIBREAC_REF|git clone[^|]*libreac|libreac\.wrap|build-with-libreac'

# --- positive controls
bounded 'BuildRequires:  pkgconfig(libreac) >= 1.7.0' 'libreac' '>=' '<' \
	&& { echo "FAIL: self-check: a floor with no ceiling read as bounded" >&2; exit 1; }
grep -Eq "$PIN" <<<'LIBREAC_REF: v1.7.0' \
	|| { echo "FAIL: self-check: the pin pattern cannot see a planted LIBREAC_REF" >&2; exit 1; }

spec="$ROOT/packaging/reac-pw.spec"
ctl="$ROOT/debian/control"
if [ ! -f "$spec" ] || [ ! -f "$ctl" ]; then
	echo "FAIL: $spec or $ctl missing" >&2
	exit 1
fi
stext=$(cat "$spec")
ctext=$(cat "$ctl")

for n in 'pkgconfig[(]libreac[)]' 'pkgconfig[(]libreac-transport[)]' 'libreac-tests'; do
	[ -n "$(decl "$stext" "^BuildRequires: +$n")" ] || { say "spec: no BuildRequires for $n"; continue; }
	bounded "$(decl "$stext" "^BuildRequires: +$n")" "$n" '>=' '<' \
		|| say "spec: BuildRequires $n lacks a floor (>=) or a ceiling (<)"
done
for n in 'libreac' 'libreac-transport'; do
	[ -n "$(decl "$stext" "^Requires: +$n ")" ] || { say "spec: no Requires for $n"; continue; }
	bounded "$(decl "$stext" "^Requires: +$n ")" "$n" '>=' '<' \
		|| say "spec: Requires $n lacks a floor (>=) or a ceiling (<)"
done
for n in 'libreac-dev' 'libreac-transport-dev' 'libreac-tests'; do
	[ -n "$(decl "$ctext" "^ *$n [(]")" ] || { say "control: no Build-Depends for $n"; continue; }
	bounded "$(decl "$ctext" "^ *$n [(]" | sed 's/(//')" "$n" '>=' '<<' \
		|| say "control: Build-Depends $n lacks a floor (>=) or a ceiling (<<)"
done

# --- no libreac source pin, in the tracked tree (the tarball case: the files that exist)
hits=$(cd "$ROOT" && {
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git ls-files; else find . -type f | sed 's,^\./,,'; fi
} | grep -Ev '^(docs/|facts/)' | grep -v "^tests/$SELF$" | while read -r f; do
	[ -f "$f" ] || continue
	case "$f" in *libreac.wrap|*build-with-libreac.sh) echo "$f: exists"; continue ;; esac
	grep -HnE "$PIN" "$f" 2>/dev/null
done)
[ -z "$hits" ] || { say "libreac is pinned or fetched as source:"; echo "$hits" >&2; }

[ "$fail" = 0 ] && echo "OK: libreac is bounded in spec and control and pinned nowhere"
exit "$fail"
