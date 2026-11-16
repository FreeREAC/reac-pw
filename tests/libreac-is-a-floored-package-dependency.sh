#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# NO PINS (FreeREAC/reac-pw#23): libreac is declared in the packaging and nowhere else, as a
# FLOOR and nothing above it.
#
# libreac, libreac-transport and libreac-tests (fake_box) are declared in packaging/reac-pw.spec
# and debian/control with a floor (the first release whose behaviour this daemon needs),
# installed from the channel by `dnf builddep` / `apt-get build-dep`, and found by pkg-config.
# THERE IS NO CEILING: libreac's soname moves with every release, so the soname dependency
# rpm and dpkg generate from the link already ties each reac-pw build to the libreac it was
# built against, and a hand-written upper range would only restate that, by hand, wrongly.
# This fails when
#   - a package the build or the tests need has no floor, in the spec's BuildRequires, in its
#     Requires, or in debian/control's Build-Depends;
#   - any of them still carries a ceiling (`<` in the spec, `<<` in control);
#   - a tracked file clones, fetches or pins a libreac source: LIBREAC_REF, a `git clone` of
#     FreeREAC/libreac, the subprojects/libreac.wrap fallback, tools/build-with-libreac.sh.
#
# A SWEEP THAT CANNOT SEE PROVES IT CAN: the floor and ceiling patterns must read planted lines,
# and the pin pattern must match a planted line.
#
#   tests/libreac-is-a-floored-package-dependency.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SELF=$(basename "$0")
fail=0
say() { echo "FAIL: $*" >&2; fail=1; }

# decl <text> <regex>: the lines of <text> that match
decl() { awk -v re="$2" '$0 ~ re' <<<"$1"; }
floored() { grep -Eq "$2 *[)]? *>= " <<<"$1"; }
ceilinged() { grep -Eq "$2 *[)]? *<<? " <<<"$1"; }
PIN='LIBREAC_REF|git clone[^|]*libreac|libreac\.wrap|build-with-libreac'

# --- positive controls
floored 'BuildRequires:  pkgconfig(libreac) >= 1.7.0' 'libreac' \
	|| { echo "FAIL: self-check: a floor was not read as a floor" >&2; exit 1; }
ceilinged 'BuildRequires:  pkgconfig(libreac) < 1.9' 'libreac' \
	|| { echo "FAIL: self-check: a ceiling was not read as a ceiling" >&2; exit 1; }
ceilinged 'libreac-dev << 1.9~,' 'libreac-dev' \
	|| { echo "FAIL: self-check: a control ceiling was not read as a ceiling" >&2; exit 1; }
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
	floored "$(decl "$stext" "^BuildRequires: +$n")" "$n" || say "spec: BuildRequires $n has no floor (>=)"
	ceilinged "$(decl "$stext" "^BuildRequires: +$n")" "$n" && say "spec: BuildRequires $n has a ceiling (<): the soname already ties the build"
done
for n in 'libreac' 'libreac-transport'; do
	[ -n "$(decl "$stext" "^Requires: +$n ")" ] || { say "spec: no Requires for $n"; continue; }
	floored "$(decl "$stext" "^Requires: +$n ")" "$n" || say "spec: Requires $n has no floor (>=)"
	ceilinged "$(decl "$stext" "^Requires: +$n ")" "$n" && say "spec: Requires $n has a ceiling (<): the soname already ties the install"
done
for n in 'libreac-dev' 'libreac-transport-dev' 'libreac-tests'; do
	[ -n "$(decl "$ctext" "^ *$n [(]")" ] || { say "control: no Build-Depends for $n"; continue; }
	floored "$(decl "$ctext" "^ *$n [(]" | sed 's/(//')" "$n" || say "control: Build-Depends $n has no floor (>=)"
	ceilinged "$(decl "$ctext" "^ *$n [(]" | sed 's/(//')" "$n" && say "control: Build-Depends $n has a ceiling (<<)"
done

# ONE FLOOR (2026-10-28). Every site names the same libreac floor: meson.build's two
# dependency() floors, the spec's BuildRequires and Requires, control's -dev Build-Depends
# (libreac-tests keeps its older floor on purpose).
FLOOR_RE='^(Build)?Requires: +(pkgconfig[(])?libreac(-transport)?[)]? +>='
ver() { grep -oE '[0-9]+(\.[0-9]+){0,2}' | head -1; }
[ "$(decl 'Requires:       libreac >= 1.9.0' "$FLOOR_RE" | sed 's/.*>=//' | ver)" = 1.9.0 ] \
	|| { echo "FAIL: self-check: the floor pattern cannot read a planted Requires line" >&2; exit 1; }
floors=$( {
	grep -oE "dependency\('libreac(-transport)?', version : '>= *[0-9.]+'" "$ROOT/meson.build" | sed "s/.*>=//" | grep -oE '[0-9.]+'
	decl "$stext" "$FLOOR_RE" | while read -r l; do sed 's/.*>=//' <<<"$l" | ver; done
	decl "$ctext" '^ *libreac(-transport)?-dev [(]>=' | while read -r l; do sed 's/.*>=//' <<<"$l" | ver; done
} | sort -u)
[ "$(wc -l <<<"$floors")" = 1 ] && [ "$(wc -w <<<"$floors")" = 1 ] \
	|| say "the libreac floor is not ONE value across meson.build, the spec and control: $(tr '\n' ' ' <<<"$floors")"

# --- no libreac source pin, in the tracked tree (the tarball case: the files that exist)
# ONE FLOOR, ONE CEILING (2026-10-28). Every site names the same libreac range: meson.build's
# two dependency() floors, the spec's BuildRequires and Requires, control's -dev Build-Depends;
# the ceilings add libreac-tests (its floor is older on purpose). 1.0.42's first RPM raised the
# BuildRequires to 1.10 and left `Requires: libreac < 1.10` in place -- bounded, so this gate
# passed -- and dnf then downgraded libreac under the running daemon to satisfy it.
FLOOR_RE='^(Build)?Requires: +(pkgconfig[(])?libreac(-transport)?[)]? +>='
CEIL_RE='^(Build)?Requires: +(pkgconfig[(])?libreac[-a-z]*[)]? +<'
ver() { grep -oE '[0-9]+(\.[0-9]+){0,2}' | head -1; }
[ "$(decl 'Requires:       libreac >= 1.9.0' "$FLOOR_RE" | sed 's/.*>=//' | ver)" = 1.9.0 ] \
	|| { echo "FAIL: self-check: the floor pattern cannot read a planted Requires line" >&2; exit 1; }
[ "$(decl 'Requires:       libreac-transport < 1.10' "$CEIL_RE" | sed 's/.*<//' | ver)" = 1.10 ] \
	|| { echo "FAIL: self-check: the ceiling pattern cannot read a planted Requires line" >&2; exit 1; }
floors=$( {
	grep -oE "dependency\('libreac(-transport)?', version : '>= *[0-9.]+'" "$ROOT/meson.build" | sed "s/.*>=//" | grep -oE '[0-9.]+'
	decl "$stext" "$FLOOR_RE" | while read -r l; do sed 's/.*>=//' <<<"$l" | ver; done
	decl "$ctext" '^ *libreac(-transport)?-dev [(]>=' | while read -r l; do sed 's/.*>=//' <<<"$l" | ver; done
} | sort -u)
ceilings=$( {
	decl "$stext" "$CEIL_RE" | while read -r l; do sed 's/.*<//' <<<"$l" | ver; done
	decl "$ctext" '^ *libreac[-a-z]* [(]<<' | while read -r l; do sed 's/.*<<//' <<<"$l" | ver; done
} | sort -u)
[ "$(wc -l <<<"$floors")" = 1 ] && [ "$(wc -w <<<"$floors")" = 1 ] \
	|| say "the libreac floor is not ONE value across meson.build, the spec and control: $(tr '\n' ' ' <<<"$floors")"
[ "$(wc -l <<<"$ceilings")" = 1 ] && [ "$(wc -w <<<"$ceilings")" = 1 ] \
	|| say "the libreac ceiling is not ONE value across the spec and control: $(tr '\n' ' ' <<<"$ceilings")"
# THE CEILING IS THE FLOOR'S NEXT MINOR. A ceiling at the next major (< 2) takes in libreac
# minors whose behaviour the tests have never seen (1.11 moved the master to peer binding,
# while reac_master and reac_hunt still asserted the old behaviour).
want_ceil=$(awk -F. '{ print $1 "." $2 + 1 }' <<<"$floors")
[ "$ceilings" = "$want_ceil" ] \
	|| say "the libreac ceiling $ceilings is not the floor's next minor $want_ceil (floor $floors)"

hits=$(cd "$ROOT" && {
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git ls-files; else find . -type f | sed 's,^\./,,'; fi
} | grep -Ev '^(docs/|facts/)' | grep -v "^tests/$SELF$" | while read -r f; do
	[ -f "$f" ] || continue
	case "$f" in *libreac.wrap|*build-with-libreac.sh) echo "$f: exists"; continue ;; esac
	grep -HnE "$PIN" "$f" 2>/dev/null
done)
[ -z "$hits" ] || { say "libreac is pinned or fetched as source:"; echo "$hits" >&2; }

[ "$fail" = 0 ] && echo "OK: libreac is a floor in spec and control, one floor everywhere, no ceiling, pinned nowhere"
exit "$fail"
