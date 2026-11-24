#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE DEVEL PACKAGE SHIPS THE HEADERS A CONSUMER PINS (FreeREAC/reac-pw#108).
#
# openmixer's conformance test pins the head-amp property prefix to reac_headamp_prop.h. With one
# package and no development package it could read that header only from a source checkout; a
# container verify has none. The facts headers are now shipped by reac-pw-devel (RPM) and
# reac-pw-dev (DEB) under /usr/include/reac-pw, with reac-pw.pc naming that directory.
#
# This fails when
#   - a pinned header (PINNED below) is not in the spec's `%files devel` list or in
#     debian/reac-pw-dev.install, or is not installed by meson.build;
#   - a header the pinned one includes by a quoted include (the closure, walked from src/ and facts/)
#     is missing from any of the three, so the installed header would not compile;
#   - the spec has no `%package devel`, debian/control has no `Package: reac-pw-dev`, or the .pc is
#     not listed in both;
#   - a Requires/Depends of the devel package carries anything but a floor (>=): no pin (=), no cap.
# With RPM_DEVEL=<built reac-pw-devel rpm> it also checks `rpm -qlp` of the built package.
#
# A SWEEP THAT CANNOT SEE PROVES IT CAN: the closure walker runs on a planted header first.
#
#   tests/devel-package-ships-the-pinned-headers.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
PINNED="reac_headamp_prop.h"
fail=0
say() { echo "FAIL: $*" >&2; fail=1; }

# closure <dir>... <header>: the header and every quoted include it reaches, found in the dirs
closure() {
	local want=("${@: -1}") seen="" h f inc
	set -- "${@:1:$#-1}"
	while [ "${#want[@]}" -gt 0 ]; do
		h="${want[0]}"; want=("${want[@]:1}")
		case " $seen " in *" $h "*) continue ;; esac
		f=""
		for d in "$@"; do [ -f "$d/$h" ] && { f="$d/$h"; break; }; done
		[ -n "$f" ] || continue          # an angle include, or a libreac header: not ours
		seen="$seen $h"
		while read -r inc; do want+=("$inc"); done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$f")
	done
	tr ' ' '\n' <<<"$seen" | sed '/^$/d' | sort
}

# --- positive control: the walker follows a planted chain and ignores an angle include
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
printf '#include "b.h"\n#include <x.h>\n' >"$t/a.h"; printf '#include "c.h"\n' >"$t/b.h"; : >"$t/c.h"
[ "$(closure "$t" a.h | tr '\n' ' ')" = "a.h b.h c.h " ] \
	|| { echo "FAIL: self-check: the closure walker cannot follow a planted include chain" >&2; exit 1; }

spec="$ROOT/packaging/reac-pw.spec"
ctl="$ROOT/debian/control"
dinst="$ROOT/debian/reac-pw-dev.install"
mes="$ROOT/meson.build"
[ -f "$spec" ] && [ -f "$ctl" ] || { echo "FAIL: spec or debian/control missing" >&2; exit 1; }

headers=$(closure "$ROOT/src" "$ROOT/facts" "$PINNED")
grep -qx "$PINNED" <<<"$headers" || { echo "FAIL: the pinned $PINNED is not in src/ or facts/" >&2; exit 1; }

# --- RPM: %files devel
grep -Eq '^%package +devel' "$spec" || say "spec: no '%package devel'"
files=$(awk '/^%files +devel/{on=1; next} /^%(files|post|pre|changelog|description|package)/{on=0} on' "$spec")
[ -n "$files" ] || say "spec: no '%files devel' list"
for h in $headers; do
	grep -Eq "^[^#]*/reac-pw/$h\$" <<<"$files" \
		|| say "spec: %files devel does not list the header $h"
done
grep -Eq '^[^#]*/pkgconfig/reac-pw\.pc$' <<<"$files" || say "spec: %files devel does not list reac-pw.pc"

# --- DEB: the .install list of reac-pw-dev
grep -Eq '^Package: reac-pw-dev$' "$ctl" || say "control: no 'Package: reac-pw-dev'"
if [ -f "$dinst" ]; then
	for h in $headers; do
		grep -Eq "^[^#]*usr/include/reac-pw/$h\$" "$dinst" || say "debian/reac-pw-dev.install does not list the header $h"
	done
	grep -Eq '^[^#]*pkgconfig/reac-pw\.pc$' "$dinst" || say "debian/reac-pw-dev.install does not list reac-pw.pc"
else
	say "debian/reac-pw-dev.install is missing"
fi

# --- meson installs what the lists name
for h in $headers; do
	if ! grep -Eq "(src|facts)/$h'" "$mes" || ! grep -q "install_headers" "$mes"; then
		say "meson.build does not install_headers the header $h"
	fi
done
# (a bash match, not a grep pattern: a grep -q pattern in tests/ is a claim about a daemon line,
# which tests/asserted-lines-have-a-producer.py holds to a producer in src/)
[[ $(<"$mes") == *"pkgconfig').generate("* ]] || say "meson.build generates no reac-pw.pc"

# --- floors only in the devel package's own dependencies
dspec=$(awk '/^%package +devel/{on=1; next} /^%(description|files|prep|build|install|check|package)/{on=0} on' "$spec" | grep -E '^Requires:')
# a Requires line may carry only a bare name or `name >= version`
if grep -Ev '^Requires:[[:space:]]+[^[:space:]<=>]+([[:space:]]+>=[[:space:]]+[^[:space:]]+)?[[:space:]]*$' <<<"$dspec" | grep -q .; then
	say "spec: a devel Requires is a pin or a cap: $dspec"
fi
dctl=$(awk '/^Package: reac-pw-dev$/{on=1; next} /^Package:/{on=0} on' "$ctl")
if grep -Eq '[(][[:space:]]*(<<|<|=|>>)' <<<"$dctl"; then say "control: a reac-pw-dev Depends is a pin or a cap"; fi
# positive control: the same patterns see a planted pin and a planted cap
grep -Ev '^Requires:[[:space:]]+[^[:space:]<=>]+([[:space:]]+>=[[:space:]]+[^[:space:]]+)?[[:space:]]*$' <<<'Requires: libreac-devel = 1.11.0' | grep -q . \
	|| { echo "FAIL: self-check: a planted Requires pin read as a floor" >&2; exit 1; }
grep -Eq '[(][[:space:]]*(<<|<|=|>>)' <<<' libreac-dev (<< 1.12~)' \
	|| { echo "FAIL: self-check: a planted Depends cap read as a floor" >&2; exit 1; }

# --- the devel package depends on libreac's development files, and the daemon package still installs
# the daemon (with two binary packages meson installs into debian/tmp, so the .install must name it).
# Ported from lane/reac-pw-devel-package (#133): checks main's test lacked. Each is also run against a
# sabotaged copy of the tree and must go red there (a guard never broken is decoration).
extra() { # <root>: prints each failure
	local r="$1" sp ct
	sp="$r/packaging/reac-pw.spec"; ct="$r/debian/control"
	awk '/^%package +devel/{on=1; next} /^%(description|files|prep|build|install|check|package)/{on=0} on' "$sp" \
		| grep -Eq '^Requires:[[:space:]]+(pkgconfig\(libreac\)|libreac-devel)[[:space:]]+>=' \
		|| echo "spec: reac-pw-devel does not require libreac's development files at a floor"
	awk '/^Package: reac-pw-dev$/{on=1; next} /^Package:/{on=0} on' "$ct" | grep -Eq 'libreac-dev[[:space:]]*\(>=' \
		|| echo "control: reac-pw-dev does not depend on libreac-dev (>= a floor)"
	[[ $(<"$r/meson.build") =~ subdirs[[:space:]]*:[[:space:]]*\'reac-pw\' ]] \
		|| echo "meson.build: reac-pw.pc is not generated with subdirs 'reac-pw'"
	grep -qx 'usr/bin/reac-pw' "$r/debian/reac-pw.install" \
		|| echo "debian/reac-pw.install does not take usr/bin/reac-pw (meson installs into debian/tmp)"
}
out=$(extra "$ROOT"); [ -z "$out" ] || { sed 's/^/FAIL: /' <<<"$out" >&2; fail=1; }
sab() { # <what> <file> <sed expr>: the same checks on a copy broken one way must report
	local c; c=$(mktemp -d)
	cp -r "$ROOT/packaging" "$ROOT/debian" "$ROOT/meson.build" "$c/"
	sed -i "$3" "$c/$2"
	[ -n "$(extra "$c")" ] || { echo "FAIL: self-check: $1 stayed green" >&2; fail=1; }
	rm -rf "$c"
}
sab "a devel Requires without libreac" packaging/reac-pw.spec '/^%package devel/,/^%description devel/{/^Requires:.*libreac/d}'
sab "a reac-pw-dev without libreac-dev" debian/control '/^Package: reac-pw-dev$/,${/libreac-dev/d}'
sab "a .pc without its subdirs" meson.build "s/subdirs : 'reac-pw'/subdirs : 'x'/"
sab "a daemon install dropped from reac-pw.install" debian/reac-pw.install '/^usr\/bin\/reac-pw$/d'

# --- optional: the built package's file list
if [ -n "${RPM_DEVEL:-}" ]; then
	list=$(rpm -qlp "$RPM_DEVEL") || say "rpm -qlp $RPM_DEVEL failed"
	for h in $headers; do grep -qx "/usr/include/reac-pw/$h" <<<"$list" || say "built RPM lacks /usr/include/reac-pw/$h"; done
	grep -Eqx '/usr/share/pkgconfig/reac-pw\.pc' <<<"$list" || say "built RPM lacks reac-pw.pc"
fi

[ "$fail" = 0 ] && echo "OK: reac-pw-devel and reac-pw-dev list $(tr '\n' ' ' <<<"$headers")and reac-pw.pc, floors only"
exit "$fail"
