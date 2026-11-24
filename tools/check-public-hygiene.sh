#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE PUBLIC TREE STAYS PUBLIC-SHAPED. What this repo's published tree must not carry:
#
#   1. a compiled Python file (__pycache__/, *.pyc, *.pyo): it embeds the path of the machine
#      that built it, and is not source.
#   2. a source file (*.c *.h *.py *.sh) without the SPDX-License-Identifier and Copyright
#      line pair in its first lines. Every file states its licence and holder the same way.
#   3. a citation of an internal document by path: specs, notes and audits live in the private
#      ops repository, so a file may cite one by its bare slug (no directory, no `.md`) and
#      never as a path (design/ or audits/ under docs) or a dated filename that resolves to
#      nothing in a public checkout. The internal directories themselves are skipped while
#      they are still in this tree.
#   4. a README that builds: README.md is the pitch and the install, with no build command in
#      its code blocks, and BUILDING.md exists and carries `meson setup`.
#
# Checks 1 and 2 read the git index (`git ls-files`), so an untracked scratch file is not
# judged; with no git checkout (the RPM %check unpacks a tarball) they are skipped and the
# run says so. Checks 3 and 4 read the files on disk and run anywhere.
#
#   tools/check-public-hygiene.sh [repo-root]
#
# Wired as the `public_hygiene` meson test (the CI suite runs it inside its first meson run).
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT" || exit 1
in_git=1
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || in_git=0

HEAD_LINES=8
bad=0

# ---- 3. internal documents are cited by slug, never by path ------------------------------
# The patterns are assembled so this file does not match its own.
DOCS="docs""/"
internal_path="\\b${DOCS}(design|audits)/"
dated_doc='\b[0-9]{4}-[0-9]{2}-[0-9]{2}-[A-Za-z0-9-]+\.m''d\b'
seen=0
while IFS= read -r -d '' f; do
	f="${f#./}"
	case "$f" in "${DOCS}design"/*|"${DOCS}audits"/*|"${DOCS}RIG-MASTERS.txt") continue ;; esac
	seen=$((seen + 1))
	hit=$(grep -InE "$internal_path" -- "$f" | sed "s|^|$f:|" || true)
	if [ -n "$hit" ]; then
		echo "FAIL: names an internal-doc path; cite the bare slug:"
		echo "$hit" | sed 's/^/    /'
		bad=1
	fi
	hit=$(grep -InE "$dated_doc" -- "$f" | sed "s|^|$f:|" || true)
	if [ -n "$hit" ]; then
		echo "FAIL: names a dated document by filename; drop the extension:"
		echo "$hit" | sed 's/^/    /'
		bad=1
	fi
done < <(find . -type d \( -name .git -o -name subprojects -o -name logs -o -name __pycache__ \
		-o -name 'build*' -o -name '_build*' \
		-o \( -name '.*' -a ! -name . -a ! -name .github \) \) -prune -o -type f -print0 | sort -z)
[ "$seen" -gt 0 ] || { echo "FAIL: no files found under $ROOT"; bad=1; }

# ---- 4. the README builds nothing; BUILDING.md carries the build ---------------------------
if [ ! -s README.md ]; then
	echo "FAIL: README.md: missing"
	bad=1
else
	readme_build=$(awk '/^[[:space:]]*```/ { fenced = !fenced; next }
		fenced && /^[[:space:]]*(sudo[[:space:]]+)?(meson|ninja|make|cmake|rpmbuild|pnpm[[:space:]]+build|tools\/build[A-Za-z0-9_.-]*|packaging\/build[A-Za-z0-9_.-]*)([[:space:]]|$)/ { print "README.md:" NR ": " $0 }' README.md)
	if [ -n "$readme_build" ]; then
		echo "FAIL: a build command belongs in BUILDING.md:"
		echo "$readme_build" | sed 's/^/    /'
		bad=1
	fi
fi
if ! grep -qE '^[[:space:]]*meson[[:space:]]+setup\b' BUILDING.md 2>/dev/null; then
	echo "FAIL: BUILDING.md: missing, or carries no \`meson setup\` step"
	bad=1
fi

if [ "$in_git" = 0 ]; then
	echo "NOTE: $ROOT is not a git checkout; the tracked-file checks (compiled Python, SPDX headers) were skipped"
	[ "$bad" = 0 ] && echo "OK: $seen public files cite internal documents by slug; README builds nothing"
	exit $bad
fi

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
	echo "OK: no compiled Python tracked; all $n source files carry the SPDX + Copyright header; $seen public files cite internal documents by slug; README builds nothing"
fi
exit $bad
