#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# ONE DOOR TO SCHED_FIFO (issue #31).
#
# The ladder law in reac_rt.h only holds while reac_rt.c is the only place that
# raises a thread's scheduling class. Two RT threads already existed — the master
# pacer and the slave upstream engine — and both had spelled the priority
# themselves, as a literal, which is how one of them ended up two full tiers over
# the PipeWire graph with nothing to notice. A third thread added the same way
# would reopen the defect and every unit test would stay green.
#
# So: sched_setscheduler / pthread_setschedparam / pthread_attr_setschedparam
# appear in reac_rt.c and nowhere else under src/. The check refuses to run
# blind — it first proves it CAN see the call in the file that legitimately has
# it, because a sweep that finds nothing and a sweep that is broken read exactly
# alike.
set -o pipefail
src="${1:?usage: rt-prio-single-door.sh SRCDIR}/src"
door="$src/reac_rt.c"
# The CALL, not the word: a doc comment naming the syscall is not a door.
pattern='(sched_setscheduler|pthread_setschedparam|pthread_attr_setschedparam)[[:space:]]*\('

# Positive control: the sweep must be able to detect presence.
if ! grep -Eq "$pattern" "$door"; then
	echo "FAIL: the sweep found no scheduling call in $door — either the door" >&2
	echo "      moved or this check is broken; it cannot report absence either way." >&2
	exit 1
fi

offenders=$(grep -rlE "$pattern" "$src" --include='*.c' --include='*.h' | grep -v '/reac_rt\.[ch]$')
if [ -n "$offenders" ]; then
	echo "FAIL: SCHED_FIFO is set outside reac_rt.c:" >&2
	echo "$offenders" >&2
	echo "      Resolve the priority with reac_rt_prio_resolve() and raise the" >&2
	echo "      thread with reac_rt_thread_go(); see the ladder in reac_rt.h." >&2
	exit 1
fi

echo "rt-prio-single-door: OK (reac_rt.c is the only door to SCHED_FIFO)"
