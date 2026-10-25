#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# make fuzz-records: EVERY CHECK NAMES THE BUG IT CATCHES, AND CATCHES IT. Each abort() of
# fuzz/ carries in its comment one or more records, a one-line change of src/ and the seed
# it shows on,
#
#   mutant: <path>:<line> `<old>` -> `<new>`
#   input: <seed of the harness's corpus>
#
# or one line `not a property: <reason>` for a stop after an allocation of the harness's
# own fails (fuzz/checks.awk reads them as OSS Verify's tools/check does). A check with
# neither, or with lines not of that form, turns this red: the ratchet. Each record is then
# proved alone: the harness, plain and native with fuzz/standalone.c for its driver, exits
# 0 on the seed as the tree is; with the change made (`<old>` must occur once on that line)
# and the harness built again it exits 134 on the seed, and the abort() it reached is the
# record's own, which each abort() of the harness names on stderr in this build. So is
# each patch of fuzz/sabotage/, applied alone: the seed its header names aborts at the
# check it names. The source is restored before each run and on every exit path.
#
# Usage: FUZZ_BUILD=build/fuzz records.sh <harness>...
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
B=${FUZZ_BUILD:-build/fuzz}/records
HARNESSES="$*"
SEP=$(printf '\037')
mkdir -p "$B/base"
# an aborting run leaves no core in the tree
ulimit -c 0

# each abort() of a harness says which it is before it aborts; `(abort)` is the function,
# not this macro
cat >"$B/record-abort.h" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
static inline void fuzz_record_abort(const char *file, int line)
{
	fprintf(stderr, "fuzz-records: abort at %s:%d\n", file, line);
	fflush(stderr);
	(abort)();
}
#define abort() fuzz_record_abort(__FILE__, __LINE__)
EOF

# build fuzz_<harness> into <dir>
build() {
	FUZZ_EXTRA="-include $B/record-abort.h" fuzz/build.sh plain "$1" "$2/fuzz_$1"
}

saved=
applied=
restore() {
	[ -n "$saved" ] && cp "$B/orig" "$saved"
	[ -n "$applied" ] && git apply -R "$applied"
	saved=
	applied=
}
trap 'restore' EXIT
trap 'restore; exit 130' INT TERM

# the ratchet: every abort() of fuzz/ has records or is not a property
sources=$(ls fuzz/*.c fuzz/*.h)
bad=$(for f in $sources; do awk '{ print NR "|0|" $0 }' "$f" | awk -v file="$f" -f fuzz/checks.awk; done |
	awk -F: '$5 == "NONE" || $5 == "BAD" { print "  " $1 ":" $3 " (" $5 ")" }')
if [ -n "$bad" ]; then
	echo "$bad"
	echo "make fuzz-records: RED -- checks with no record and no 'not a property' line, or records not of the form"
	exit 1
fi

for h in $HARNESSES; do
	build "$h" "$B/base" >"$B/base/$h.log" 2>&1 ||
		{ cat "$B/base/$h.log"; echo "records.sh: fuzz_$h does not build"; exit 1; }
done

n=0
red=0

# the harness as the tree is passes on the seed: else nothing a change makes it do shows
# the change
passes() {
	if [ -z "$2" ] || [ ! -f "fuzz/corpus/$2/$3" ]; then
		echo "RED  $1: no seed $3 in fuzz/corpus/"
	elif ! "$B/base/fuzz_$2" "fuzz/corpus/$2/$3" >"$B/run.log" 2>&1; then
		echo "RED  $1: the harness as the tree is fails on the seed"
	else
		return 0
	fi
	red=$((red + 1))
	return 1
}

# the changed tree built into $B/mutant, the source restored, then the seed run: 134, at
# the abort() of `want`
judge() {
	name=$1 h=$2 seed=$3 want=$4
	mkdir -p "$B/mutant"
	build "$h" "$B/mutant" >"$B/mutant/build.log" 2>&1
	built=$?
	restore
	if [ "$built" -ne 0 ]; then
		echo "RED  $name: the change does not build"
		red=$((red + 1))
		return
	fi
	# in a subshell, so the shell's own word on the abort is not printed
	("$B/mutant/fuzz_$h" "fuzz/corpus/$h/$seed" >"$B/run.log" 2>&1; exit $?) 2>/dev/null
	rc=$?
	hit=$(sed -n 's/^fuzz-records: abort at //p' "$B/run.log" | tail -n 1)
	if [ "$rc" -eq 134 ] && [ "$hit" = "$want" ]; then
		echo "ok   $name"
	else
		echo "RED  $name: exit $rc, ${hit:-no check}${hit:+ aborted}"
		red=$((red + 1))
	fi
}

# the harness of a check: its own file's, or for a file the harnesses share, the one
# whose corpus holds the seed
harness_of() {
	case $1 in
	fuzz/fuzz_*.c)
		h=$(basename "$1" .c)
		echo "${h#fuzz_}"
		return ;;
	esac
	for k in $HARNESSES; do
		[ -f "fuzz/corpus/$k/$2" ] && { echo "$k"; return; }
	done
}

for f in $sources; do
	awk '{ print NR "|0|" $0 }' "$f" | awk -v file="$f" -v records=1 -v sep="$SEP" -f fuzz/checks.awk
done >"$B/records.txt"
while IFS=$SEP read -r file at path line old new seed; do
	h=$(harness_of "$file" "$seed")
	case " $HARNESSES " in *" $h "*) ;; *) continue ;; esac
	n=$((n + 1))
	name="$file:$at $path:$line on $seed"
	passes "$name" "$h" "$seed" || continue
	cp "$path" "$B/orig"
	saved=$path
	if ! O=$old N=$new awk -v l="$line" '
		NR == l { s = $0; out = ""; k = 0
		          while ((i = index(s, ENVIRON["O"])) > 0) { k++; out = out substr(s, 1, i - 1) ENVIRON["N"]
		                                                     s = substr(s, i + length(ENVIRON["O"])) }
		          $0 = out s; if (k != 1) bad = 1 }
		{ print }
		END { exit bad }' "$B/orig" >"$path"; then
		restore
		echo "RED  $name: \`$old\` is not once on $path:$line"
		red=$((red + 1))
		continue
	fi
	judge "$name" "$h" "$seed" "$file:$at"
done <"$B/records.txt"
records=$n

# the breakage patches of fuzz/sabotage/, each the same way: the check and the seed its
# header names
for p in fuzz/sabotage/*.patch; do
	[ -f "$p" ] || continue
	h=$(basename "$p" .patch)
	h=${h%%-*}
	case " $HARNESSES " in *" $h "*) ;; *) continue ;; esac
	n=$((n + 1))
	at=$(sed -n "s|^Breaks the check of \(fuzz/fuzz_$h\.c:[0-9]*\) (.*)\.\$|\1|p" "$p" | head -n 1)
	seed=$(sed -n "s|^Caught by the seed input \"\([^\"]*\)\" of fuzz_$h\.\$|\1|p" "$p" | head -n 1)
	name="$at $p on $seed"
	if [ -z "$at" ]; then
		echo "RED  $p: its header names no check of fuzz/fuzz_$h.c"
		red=$((red + 1))
		continue
	fi
	passes "$name" "$h" "$seed" || continue
	if ! git apply "$p"; then
		echo "RED  $name: the patch does not apply"
		red=$((red + 1))
		continue
	fi
	applied=$p
	judge "$name" "$h" "$seed" "$at"
done

if [ "$n" -eq 0 ]; then
	echo "make fuzz-records: RED -- no record in fuzz/"
	exit 1
fi
if [ "$red" -ne 0 ]; then
	echo "make fuzz-records: RED -- $red of $n records and patches not met"
	exit 1
fi
echo "make fuzz-records: all $records records and $((n - records)) patches met, each by its own check on its own seed"
