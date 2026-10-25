# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE HARNESS FLOOR AND THE RECORD RATCHET (fuzz/coverage.sh, fuzz/records.sh). Reads one source of fuzz/, as the
# `llvm-cov show` of a harness prints it, `LINE|COUNT|source` (or any text as `LINE|0|source`), and prints one line for
# every abort() in it:
#
#   <file>:<guard line>:<abort line>:<count>:<records>:<the guard's first source line>
#
# The guard is the statement that decides whether the abort runs: the `if` it is the body of, or the statement it ends
# when it shares that line. <count> is how often the seed corpus executed the guard's first line; 0 is a property no
# seed tests. The abort's own count is not it: on a corpus that holds, no abort runs. Comments and string literals do
# not hold a site, so an abort() named in prose is not one.
#
# <records> is what the check's comment holds, read as OSS Verify's tools/check reads it (docs/build-contract.md, "The
# checks of a fuzz target"): the comment lines directly above the guard, those on the guard's lines and on the abort's.
# It is `records=<n>` for n pairs of lines
#
#   mutant: <path>:<line> `<old>` -> `<new>`
#   input: <seed>
#
# `not-a-property` for one line `not a property: <reason>`, and `NONE` for a check that has neither, or `BAD` when the
# lines are not that form: what the ratchet refuses. With -v records=1 it prints instead one line per record, its
# fields separated by tabs (or -v sep=<c>): <file> <abort line> <path> <line> <old> <new> <seed>.
BEGIN { FS = "|"; if (sep == "") sep = "\t" }
{
	ln = $1 + 0
	cnt = $2
	gsub(/[ \t]/, "", cnt)
	src = $0
	sub(/^[^|]*\|[^|]*\|/, "", src)
	# the line's code, its string literals kept, and its comment text
	code = ""
	cm = ""
	had = incomment
	quote = ""
	n = length(src)
	for (c = 1; c <= n; c++) {
		ch = substr(src, c, 1)
		two = substr(src, c, 2)
		if (incomment) {
			if (two == "*/") { incomment = 0; c++ }
			else cm = cm ch
			continue
		}
		if (quote != "") {
			code = code ch
			if (ch == "\\") { code = code substr(src, c + 1, 1); c++ }
			else if (ch == quote) quote = ""
			continue
		}
		if (two == "/*") { incomment = 1; had = 1; c++; cm = cm " "; continue }
		if (two == "//") { cm = cm " " substr(src, c + 2); had = 1; break }
		if (ch == "\"" || ch == "'") quote = ch
		code = code ch
	}
	gsub(/^[ \t]+|[ \t]+$/, "", code)
	C[ln] = code; N[ln] = cnt; S[ln] = src; CM[ln] = cm; HAS[ln] = had
	if (code ~ /(^|[^A-Za-z0-9_])abort\(\);/) site[++ns] = ln
}

# the first line of the statement that ends on line l
function stmt_start(l) {
	while (l > 1 && C[l - 1] != "" && C[l - 1] !~ /[;{}:]$/) l--
	return l
}

# the first of the comment lines directly above line l, with no code and no empty line between
function above(l) {
	l--
	while (l >= 1 && HAS[l] && C[l] == "") l--
	return l + 1
}

# a comment line without its markers
function clean(t) {
	sub(/\*\/[ \t]*$/, "", t)
	sub(/^[ \t\/*]+/, "", t)
	sub(/[ \t]+$/, "", t)
	return t
}

# the records of the check guarded from line s with its abort on line l: their count, or -1 for lines not of the form;
# NP is set for a `not a property:` line. With records=1, each is printed.
function read_records(s, l,    a, k, t, nrec, pending, rest, path, line, old, new, i) {
	NP = 0
	nrec = 0
	pending = ""
	for (k = above(s); k <= l; k++) {
		if (k < s && !(HAS[k] && C[k] == "")) continue
		t = clean(CM[k])
		if (pending != "") {
			if (t !~ /^input: [^ ]/) return -1
			sub(/^input: /, "", t)
			nrec++
			if (records) printf "%s%s%d%s%s%s%s\n", file, sep, l, sep, pending, sep, t
			pending = ""
			continue
		}
		if (t ~ /^mutant:/) {
			if (t !~ /^mutant: [^ `]+:[0-9]+ `[^`]+` -> `[^`]*`$/) return -1
			rest = substr(t, 9)
			i = index(rest, " ")
			path = substr(rest, 1, i - 1)
			rest = substr(rest, i + 2)
			line = path; sub(/^.*:/, "", line); sub(/:[0-9]+$/, "", path)
			old = rest; sub(/` -> `.*$/, "", old)
			new = rest; sub(/^.*` -> `/, "", new); sub(/`$/, "", new)
			pending = path sep line sep old sep new
		} else if (t ~ /^input:/)
			return -1
		else if (t ~ /^not a property: [^ ]/)
			NP = 1
	}
	if (pending != "" || (NP && nrec)) return -1
	return nrec
}

END {
	for (i = 1; i <= ns; i++) {
		l = site[i]
		s = (C[l] ~ /^abort\(\);/) ? stmt_start(l - 1) : stmt_start(l)
		r = read_records(s, l)
		if (records) continue
		st = r < 0 ? "BAD" : r > 0 ? "records=" r : NP ? "not-a-property" : "NONE"
		t = S[s]
		gsub(/^[ \t]+/, "", t)
		printf "%s:%d:%d:%s:%s:%s\n", file, s, l, N[s], st, t
	}
}
