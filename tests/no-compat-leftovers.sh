#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# NO BACKWARD-COMPATIBILITY MECHANISM IS KEPT FOR A VERSION NOBODY RUNS.
#
# Each arm is a thing that once existed only so an older install, config file or unit kept
# working, and that is gone: a notice about a retired REAC_ROLE key, the knock key's own
# refusal (the generic one covers it: tests/test_reac_segconf.c), the `ignore = yes` notice
# for the old daemon, the Conflicts= on a retired per-NIC unit, the global disable of an
# enablement packages up to 1.0.18 left behind, and a build-time floor on a libreac the
# meson floor already forbids. The script reads files, so it runs from the tarball too.
#   tests/no-compat-leftovers.sh [repo-root]
set -u
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 2
fail=0
say() { echo "FAIL: $*" >&2; fail=1; }
# the sweep must reach the files it speaks about
for f in src/main.c packaging/reac-pw.service packaging/reac-pw.spec debian/reac-pw.postinst debian/reac-pw.prerm \
         tests/test_reac_enrolment_binding.c; do
	[ -f "$f" ] || { echo "FAIL: $f missing: the sweep is broken, not the tree" >&2; exit 2; }
done
has_text() { [[ "$(<"$1")" == *"$2"* ]]; }   # a plain text test, not a pattern over source
has_text debian/reac-pw.prerm "global disable" \
	|| { echo "FAIL: the control is gone: prerm no longer carries the ordinary uninstall disable" >&2; exit 2; }

# 1 a retired REAC_ROLE key is not read to be named
grep -nE 'segment_say_env_role_retired|reac_conf_lookup\("REAC_ROLE"' src/main.c \
	&& say "row 1: src/main.c still reads REAC_ROLE only to say it is ignored"
# 3 the ignore = yes notice
grep -n 'is honoured: never listened to' src/main.c \
	&& say "row 3: src/main.c still prints the notice about \`ignore = yes\` for the old daemon"
# 4 the Conflicts= on the retired per-NIC unit
grep -nE '^Conflicts=|reac-pw-master' packaging/reac-pw.service packaging/90-reac-pw.preset \
	&& say "row 4: the unit or the preset still names the retired reac-pw-master.service"
# 5 the global disable of a <=1.0.18 leftover, in %post and postinst
awk '/^%post( |$)/{p=1;next} /^%(prep|build|install|check|files|posttrans|pre|preun|postun|changelog|package|description)( |$)/{p=0} p&&/global disable/' packaging/reac-pw.spec | grep -q . \
	&& say "row 5: the spec's %post still disables a global enablement an old package left behind"
has_text debian/reac-pw.postinst "global disable" \
	&& say "row 5: debian/postinst still disables a global enablement an old package left behind"
# 6 a compile-time floor behind the meson floor
grep -rn --exclude=no-compat-leftovers.sh 'LIBREAC_VERSION_AT_LEAST' src tests fuzz \
	&& say "row 6: a source still guards on LIBREAC_VERSION_AT_LEAST; the build's floor is meson.build's"

[ "$fail" = 0 ] && echo "OK: none of the compatibility leftovers is back"
exit "$fail"
