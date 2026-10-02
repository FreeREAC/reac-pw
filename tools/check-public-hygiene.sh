#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PUBLIC TREE STAYS PUBLIC-SHAPED. Two things this repo's published tree must not carry:
#
#   1. a compiled Python file (__pycache__/, *.pyc, *.pyo): it embeds the path of the machine
#      that built it, and is not source.
#   2. a source file (*.c *.h *.py *.sh) without the SPDX-License-Identifier and Copyright
#      line pair in its first lines. Every file states its licence and holder the same way.
#
# Reads the git index (`git ls-files`), so an untracked scratch file is not judged; a tree
# with no git checkout (the RPM %check unpacks a tarball) SKIPs with exit 77.
#
#   tools/check-public-hygiene.sh [repo-root]
#
# Wired as the `public_hygiene` meson test and as a step in .github/workflows/test.yml.
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 1
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "SKIP: $ROOT is not a git checkout"
	exit 77
fi

HEAD_LINES=8
bad=0

compiled=$(git ls-files -z | tr '\0' '\n' | grep -E '(^|/)__pycache__/|\.py[co]$') || true
if [ -n "$compiled" ]; then
	echo "FAIL: compiled Python files are tracked (git rm --cached them; __pycache__/ is ignored):"
	echo "$compiled" | sed 's/^/    /'
	bad=1
fi

headerless=0
while IFS= read -r -d '' f; do
	[ -f "$f" ] || continue
	top=$(head -n "$HEAD_LINES" "$f")
	miss=""
	grep -q 'SPDX-License-Identifier:' <<<"$top" || miss="SPDX-License-Identifier"
	grep -qE 'Copyright \(C\) [0-9]{4}' <<<"$top" || miss="${miss:+$miss, }Copyright (C)"
	if [ -n "$miss" ]; then
		[ "$headerless" = 0 ] && echo "FAIL: source files without the SPDX + Copyright header in their first $HEAD_LINES lines:"
		echo "    $f  (missing: $miss)"
		headerless=$((headerless + 1))
	fi
done < <(git ls-files -z -- '*.c' '*.h' '*.py' '*.sh')
[ "$headerless" -gt 0 ] && bad=1

if [ "$bad" = 0 ]; then
	n=$(git ls-files -- '*.c' '*.h' '*.py' '*.sh' | wc -l)
	echo "OK: no compiled Python tracked; all $n source files carry the SPDX + Copyright header"
fi
exit $bad
