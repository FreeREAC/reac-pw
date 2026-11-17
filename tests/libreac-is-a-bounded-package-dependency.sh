#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# NO PINS, NO CEILING (FreeREAC/reac-pw#23; the ceiling removed by the floors-only rule, #108/#110):
# libreac is declared in the packaging and nowhere else, and a consumer declares FLOORS ONLY.
#
# libreac, libreac-transport and libreac-tests (fake_box) are declared in packaging/reac-pw.spec
# and debian/control as a floor (>=), installed from the OpenMixer channel by `dnf builddep` /
# `apt-get build-dep`, and found by pkg-config. This fails when
#   - a package the build or the tests need has no floor, in the spec's BuildRequires, in its
#     Requires, or in debian/control's Build-Depends;
#   - ANY libreac* dependency carries a ceiling: a `<`, `<=`, `<<` or `=` in the spec or control
#     (the next minor of a library is published, installed and tested by its own release; a
#     consumer that caps it turns every release of the library into a red build of the consumer);
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

PIN='LIBREAC_REF|git clone[^|]*libreac|libreac\.wrap|build-with-libreac'

# --- positive controls
CEILING_RE='^[^#]*libreac[-a-z]*[)]? *[(]?(<|<<|<=|=|==) '
grep -Eq "$CEILING_RE" <<<'BuildRequires:  pkgconfig(libreac) < 1.12' \
	|| { echo "FAIL: self-check: a planted spec ceiling was not seen" >&2; exit 1; }
grep -Eq "$CEILING_RE" <<<' libreac-dev (<< 1.12~),' \
	|| { echo "FAIL: self-check: a planted control ceiling was not seen" >&2; exit 1; }
grep -Eq "$CEILING_RE" <<<'Requires:       libreac >= 1.11.0' \
	&& { echo "FAIL: self-check: a floor read as a ceiling" >&2; exit 1; }
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
	grep -Eq "$n *[)]? *>= " <<<"$(decl "$stext" "^BuildRequires: +$n")" \
		|| say "spec: BuildRequires $n lacks a floor (>=)"
done
for n in 'libreac' 'libreac-transport'; do
	[ -n "$(decl "$stext" "^Requires: +$n ")" ] || { say "spec: no Requires for $n"; continue; }
	grep -Eq "$n *>= " <<<"$(decl "$stext" "^Requires: +$n ")" \
		|| say "spec: Requires $n lacks a floor (>=)"
done
for n in 'libreac-dev' 'libreac-transport-dev' 'libreac-tests'; do
	[ -n "$(decl "$ctext" "^ *$n [(]")" ] || { say "control: no Build-Depends for $n"; continue; }
	grep -Eq "$n *[(]>= " <<<"$(decl "$ctext" "^ *$n [(]")" \
		|| say "control: Build-Depends $n lacks a floor (>=)"
done

# --- no libreac source pin, in the tracked tree (the tarball case: the files that exist)
# ONE FLOOR, NO CEILING. Every site names the same libreac floor: meson.build's two dependency()
# floors, the spec's BuildRequires and Requires, control's -dev Build-Depends. A ceiling anywhere
# fails: 1.0.42's first RPM raised the BuildRequires and left `Requires: libreac < 1.10`, and dnf
# then downgraded libreac under the running daemon to satisfy it; and on the day a library minor was
# published every build of its consumer went red (deb builds, 2026-10-27).
FLOOR_RE='^(Build)?Requires: +(pkgconfig[(])?libreac(-transport)?[)]? +>='
ver() { grep -oE '[0-9]+(\.[0-9]+){0,2}' | head -1; }
[ "$(decl 'Requires:       libreac >= 1.9.0' "$FLOOR_RE" | sed 's/.*>=//' | ver)" = 1.9.0 ] \
	|| { echo "FAIL: self-check: the floor pattern cannot read a planted Requires line" >&2; exit 1; }
floors=$( {
	grep -oE "dependency\('libreac(-transport)?', version : '>= *[0-9.]+'" "$ROOT/meson.build" | sed "s/.*>=//" | grep -oE '[0-9.]+'
	decl "$stext" "$FLOOR_RE" | while read -r l; do ver <<<"${l#*>=}"; done
	decl "$ctext" '^ *libreac(-transport)?-dev [(]>=' | while read -r l; do ver <<<"${l#*>=}"; done
} | sort -u)
[ "$(wc -l <<<"$floors")" = 1 ] && [ "$(wc -w <<<"$floors")" = 1 ] \
	|| say "the libreac floor is not ONE value across meson.build, the spec and control: $(tr '\n' ' ' <<<"$floors")"
# the ceiling sweep covers every dependency line of both files, comments aside
ceil=$( { grep -nE "$CEILING_RE" "$spec" | sed "s,^,spec:,"; grep -nE "$CEILING_RE" "$ctl" | sed "s,^,control:,"; } )
[ -z "$ceil" ] || { say "a libreac dependency carries a ceiling (floors only):"; echo "$ceil" >&2; }

hits=$(cd "$ROOT" && {
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git ls-files; else find . -type f | sed 's,^\./,,'; fi
} | grep -Ev '^(docs/|facts/)' | grep -v "^tests/$SELF$" | while read -r f; do
	[ -f "$f" ] || continue
	case "$f" in *libreac.wrap|*build-with-libreac.sh) echo "$f: exists"; continue ;; esac
	grep -HnE "$PIN" "$f" 2>/dev/null
done)
[ -z "$hits" ] || { say "libreac is pinned or fetched as source:"; echo "$hits" >&2; }

[ "$fail" = 0 ] && echo "OK: libreac has one floor and no ceiling in spec and control, pinned nowhere"
exit "$fail"
