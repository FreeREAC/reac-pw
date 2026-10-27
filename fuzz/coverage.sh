#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COVERAGE WITNESS (make fuzz-coverage). Each harness, built for llvm-cov without
# sanitizers, has run once over its seed corpus (-runs=0 runs the corpus and nothing else);
# this prints the line coverage of the reader functions the harness exists to drive, and
# every line of them no seed reached. A file's other functions (segconf's directory walk,
# the qdisc policy) are not the harness's and are not counted.
#
# THE FLOOR. fuzz/coverage-floor.txt holds, for each harness, the number of reader lines
# its committed corpus covers. A harness below its floor turns this red: a seed lost, a
# harness that stopped driving a function, or a reader that grew lines its corpus never
# reaches. Raise a floor when coverage rises; lower one only with the change that removed
# lines.
#
# THE PER-CHECK FLOOR. Every property check of the harness, each abort() of
# fuzz/fuzz_<harness>.c and the statement that guards it (fuzz/checks.awk), must be
# executed by at least one seed: a check no seed reaches is a property nobody tests, and
# the reader's line count cannot see it. Each such check is named by file and line and
# turns this red. So does a check whose comment holds no `mutant:` and `input:` record and
# no `not a property:` line (fuzz/records.sh proves the records).
#
# Usage: LLVM_COV=llvm-cov coverage.sh <cov build dir> <harness>...
set -eu
B=$1
shift
LLVM_COV=${LLVM_COV:-llvm-cov}
FLOOR=fuzz/coverage-floor.txt
[ -f "$FLOOR" ] || { echo "coverage.sh: no $FLOOR" >&2; exit 2; }
low=
dead=

# the reader functions each harness covers, its files then `::` then the functions; a
# static is matched by bare name
funcs() {
	case $1 in
	segconf) echo src/reac_segconf.c src/reac_declared_vlan.c :: reac_segconf_init refuse trim \
		strip_comment unquote parse_bool seg_get reac_segconf_parse_file cross_check \
		reac_segconf_parse reac_segconf_find reac_segconf_ignored reac_segconf_role \
		reac_segconf_role_file reac_segconf_ignore_file reac_segconf_model \
		reac_segconf_knock reac_segconf_declared \
		name_ok reac_declared_vlan_split reac_declared_vlan_add ;;
	pin) echo src/reac_box_pin.c :: reac_box_pin_parse ;;
	headamp) echo src/reac_headamp_prop.c :: value_pod_to_byte param_name_to_id parse_key \
		value_in_range reac_headamp_prop_parse reac_headamp_prop_parse_result ;;
	rate) echo src/reac_rate_cfg.c :: reac_rate_refuse_code reac_rate_is_closed reac_rate_bit \
		reac_rate_best_drivable reac_rate_drivable_csv reac_rate_cfg_decide value_pod_to_hz \
		reac_rate_prop_parse ;;
	role) echo src/reac_role_cfg.c :: reac_role_refuse_code reac_role_cfg_changes \
		reac_role_cfg_apply_state value_pod_to_role_flag reac_role_prop_parse ;;
	qdisc) echo src/reac_qdisc.c :: qd_take_stats2 qd_take_stats1 qd_take_qdisc \
		reac_qdisc_stats_read reac_qdisc_etf_catchup_slots ;;
	link) echo src/reac_link_alias.c src/reac_served_mark.c :: copy_attr nl_talk \
		reac_link_alias_read reac_link_alias_write reac_served_decide remember \
		reac_served_mark reac_served_unmark_all ;;
	*) echo "coverage.sh: no function list for harness '$1'" >&2; exit 2 ;;
	esac
}

for h in "$@"; do
	set -- $(funcs "$h")
	files=
	while [ "$1" != "::" ]; do files="$files $1"; shift; done
	shift
	bin=$B/fuzz_$h prof=$B/$h.profdata tot=$B/$h.covered
	echo "== fuzz_$h: $(ls fuzz/corpus/$h | wc -l) seeds"
	# Name Regions Miss Cover Lines Miss Cover ...: the listed functions only
	"$LLVM_COV" report -show-functions "$bin" -instr-profile="$prof" $files |
	awk -v want="$*" -v tot="$tot" '
		BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) keep[w[i]] = 1 }
		{ name = $1; sub(/^.*:/, "", name) }
		(name in keep) && NF >= 10 {
			printf "  %-32s lines %4d  missed %3d  %s\n", name, $5, $6, $7
			lines += $5; missed += $6; seen[name] = 1
		}
		END {
			for (k in keep) if (!(k in seen)) printf "  %-32s NOT FOUND in the report\n", k
			printf "  %-32s lines %4d  missed %3d  %.2f%%\n", "TOTAL", lines, missed,
			       lines ? 100 * (lines - missed) / lines : 0
			print lines - missed > tot
		}'
	for f in "$@"; do
		"$LLVM_COV" show "$bin" -instr-profile="$prof" -name-regex="^(.*:)?$f\$" $files 2>/dev/null |
		awk -v f="$f" -F'|' '$2 ~ /^ *0$/ { sub(/^ */, "", $1); printf "    unreached %s:%s %s\n", f, $1, $3 }'
	done
	src=fuzz/fuzz_$h.c
	"$LLVM_COV" show "$bin" -instr-profile="$prof" "$src" | awk -v file="$src" -f fuzz/checks.awk >"$B/$h.checks"
	nchk=$(wc -l <"$B/$h.checks")
	if [ "$nchk" -eq 0 ]; then
		echo "  CHECKS: no property check found in $src"
		dead="$dead $h"
	elif awk -F: '$4 == "0" && $5 != "not-a-property" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$4 == "0" && $5 != "not-a-property" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  CHECK: %s:%d no seed reaches (abort at line %d): %s\n", $1, $2, $3, t }' "$B/$h.checks"
		dead="$dead $h"
	elif awk -F: '$5 == "NONE" || $5 == "BAD" { n++ } END { exit !n }' "$B/$h.checks"; then
		awk -F: '$5 == "NONE" || $5 == "BAD" { t = $0; sub(/^([^:]*:){5}/, "", t)
			printf "  RECORD: %s:%d has no mutant: and input: record and is no stop after a failed allocation" \
				" (%s): %s\n", $1, $3, $5, t }' "$B/$h.checks"
		dead="$dead $h"
	else
		echo "  checks: all $nchk property checks of $src reached"
	fi
	covered=$(cat "$tot")
	floor=$(awk -v h="$h" '$1 == h { print $2 }' "$FLOOR")
	if [ -z "$floor" ]; then
		echo "  FLOOR: fuzz_$h has no line in $FLOOR"
		low="$low $h"
	elif [ "$covered" -lt "$floor" ]; then
		echo "  FLOOR: fuzz_$h covers $covered reader lines, below its floor of $floor"
		low="$low $h"
	else
		echo "  floor $floor: held ($covered covered)"
	fi
done

[ -z "$low" ] || echo "make fuzz-coverage: RED -- below the coverage floor:$low"
[ -z "$dead" ] || echo "make fuzz-coverage: RED -- harness checks no seed reaches, or with no record:$dead"
[ -z "$low$dead" ] || exit 1
echo "make fuzz-coverage: every harness at or above its coverage floor, every check reached and recorded"
