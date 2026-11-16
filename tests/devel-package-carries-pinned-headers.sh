#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# THE DEVEL PACKAGE CARRIES WHAT CONSUMERS PIN (FreeREAC/reac-pw#108). A consumer that pins one of
# reac-pw's public facts headers (packaging/devel-headers.txt) finds it in the INSTALLED devel package
# through pkg-config, never in a source checkout beside it. This fails when
#   - a pinned header, or a local header it includes (src/, facts/, transitively), is not in
#     meson.build's devel_headers list, which install_headers() puts under <includedir>/reac-pw;
#   - meson.build does not generate reac-pw.pc naming that directory;
#   - the spec has no reac-pw-devel subpackage owning %{_includedir}/reac-pw and reac-pw.pc, or it
#     does not require libreac's development files (the pinned headers include <reac/...>);
#   - debian/control has no reac-pw-dev package, or its .install does not take both, or reac-pw's
#     .install no longer takes the daemon (with two binary packages, meson installs into debian/tmp).
# A SWEEP THAT CANNOT SEE PROVES IT CAN: the closure must find reac_headamp_state.h under
# reac_headamp_prop.h, and a copy of the tree with that header dropped from the list must go red.
#
#   tests/devel-package-carries-pinned-headers.sh [repo-root]
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# closure <root> <header basename>: the local headers it pulls in, itself first, as src/... or facts/...
closure() {
	local root="$1" todo=("$2") seen=() f n d p
	while [ "${#todo[@]}" -gt 0 ]; do
		n="${todo[0]}"; todo=("${todo[@]:1}")
		case " ${seen[*]-} " in *" $n "*) continue ;; esac
		for d in src facts; do [ -f "$root/$d/$n" ] && { p="$d/$n"; break; }; p=""; done
		[ -n "$p" ] || { echo "MISSING:$n"; continue; }
		seen+=("$n"); echo "$p"
		while read -r f; do todo+=("$f"); done < <(sed -nE 's/^[[:space:]]*#[[:space:]]*include[[:space:]]+"([^"]+)".*/\1/p' "$root/$p")
	done
}

check() { # <root>: prints each failure, returns 1 on any
	local root="$1" bad=0 h p list spec ctl
	list="$root/packaging/devel-headers.txt"
	[ -f "$list" ] || { echo "no $list"; return 1; }
	# meson.build's devel_headers = files(...) block
	local block
	block=$(awk '/^devel_headers *= *files\(/{on=1} on{print} on&&/\)/{exit}' "$root/meson.build")
	[ -n "$block" ] || { echo "meson.build: no devel_headers = files(...) list"; bad=1; }
	grep -Eq "install_headers\(devel_headers, *subdir *: *'reac-pw'\)" "$root/meson.build" \
		|| { echo "meson.build: devel_headers is not installed under <includedir>/reac-pw"; bad=1; }
	grep -Eq "name *: *'reac-pw'" <(awk '/generate\(/{on=1} on{print} on&&/^\)/{on=0}' "$root/meson.build") \
		&& grep -Eq "subdirs *: *'reac-pw'" "$root/meson.build" \
		|| { echo "meson.build: no reac-pw.pc generated with subdirs 'reac-pw'"; bad=1; }
	while read -r h _; do
		case "$h" in ''|'#'*) continue ;; esac
		while read -r p; do
			case "$p" in MISSING:*) echo "$h includes ${p#MISSING:}, which is not in src/ or facts/"; bad=1; continue ;; esac
			grep -qF "'$p'" <<<"$block" || { echo "devel package: $p (pinned: $h) is not in meson.build's devel_headers"; bad=1; }
		done < <(closure "$root" "$h")
	done < "$list"
	spec="$root/packaging/reac-pw.spec"
	grep -Eq '^%package +devel' "$spec" || { echo "spec: no reac-pw-devel subpackage"; bad=1; }
	awk '/^%files +devel/{on=1;next} /^%(files|changelog)/{on=0} on' "$spec" | grep -qF '%{_includedir}/reac-pw/' \
		|| { echo "spec: %files devel does not own %{_includedir}/reac-pw/"; bad=1; }
	awk '/^%files +devel/{on=1;next} /^%(files|changelog)/{on=0} on' "$spec" | grep -qF '%{_libdir}/pkgconfig/reac-pw.pc' \
		|| { echo "spec: %files devel does not own reac-pw.pc"; bad=1; }
	awk '/^%package +devel/{on=1;next} /^%(description|package|prep)/{on=0} on' "$spec" | grep -Eq '^Requires: +pkgconfig\(libreac\) +>=' \
		|| { echo "spec: reac-pw-devel does not require pkgconfig(libreac) >= a floor"; bad=1; }
	ctl="$root/debian/control"
	grep -q '^Package: reac-pw-dev$' "$ctl" || { echo "debian/control: no reac-pw-dev package"; bad=1; }
	awk '/^Package: reac-pw-dev$/{on=1;next} /^Package:/{on=0} on' "$ctl" | grep -Eq 'libreac-dev \(>= ' \
		|| { echo "debian/control: reac-pw-dev does not depend on libreac-dev (>= a floor)"; bad=1; }
	grep -qx 'usr/include/reac-pw' "$root/debian/reac-pw-dev.install" 2>/dev/null \
		|| { echo "debian/reac-pw-dev.install does not take usr/include/reac-pw"; bad=1; }
	grep -qx 'usr/lib/\*/pkgconfig/reac-pw.pc' "$root/debian/reac-pw-dev.install" 2>/dev/null \
		|| { echo "debian/reac-pw-dev.install does not take reac-pw.pc"; bad=1; }
	grep -qx 'usr/bin/reac-pw' "$root/debian/reac-pw.install" \
		|| { echo "debian/reac-pw.install does not take usr/bin/reac-pw (meson installs into debian/tmp)"; bad=1; }
	return $bad
}

# --- positive controls
grep -qx 'src/reac_headamp_state.h' <<<"$(closure "$ROOT" reac_headamp_prop.h)" \
	|| { echo "FAIL: self-check: the closure does not find reac_headamp_state.h under reac_headamp_prop.h" >&2; exit 1; }
out=$(check "$ROOT")
if [ -n "$out" ]; then
	sed 's/^/FAIL: /' <<<"$out" >&2
	exit 1
fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp -r "$ROOT/src" "$ROOT/facts" "$ROOT/packaging" "$ROOT/debian" "$ROOT/meson.build" "$T/"
sed -i "/'src\/reac_headamp_state.h'/d" "$T/meson.build"
check "$T" >/dev/null && { echo "FAIL: self-check: a header dropped from devel_headers stayed green" >&2; exit 1; }
echo "devel package: every pinned header and what it includes is installed, with reac-pw.pc"
