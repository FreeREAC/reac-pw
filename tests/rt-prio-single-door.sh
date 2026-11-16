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
# reac_rt.c ITSELF moved to libreac-transport
# (2026-09-11-reac-transport-library) — the door is no longer under this repo's src/ at all,
# and that the door exists and is the one is libreac-transport's own test (test_reac_rt). The
# invariant this repo enforces is the half that matters most HERE: reac-pw's OWN src/ never
# spells SCHED_FIFO a second way.
#
# So: sched_setscheduler / pthread_setschedparam / pthread_attr_setschedparam appear
# nowhere under this repo's src/.
set -o pipefail
src="${1:?usage: rt-prio-single-door.sh SRCDIR}/src"
# The CALL, not the word: a doc comment naming the syscall is not a door.
pattern='(sched_setscheduler|pthread_setschedparam|pthread_attr_setschedparam)[[:space:]]*\('

offenders=$(grep -rlE "$pattern" "$src" --include='*.c' --include='*.h' || true)
if [ -n "$offenders" ]; then
	echo "FAIL: SCHED_FIFO is set in reac-pw's own src/, outside libreac-transport's reac_rt.c:" >&2
	echo "$offenders" >&2
	echo "      Resolve the priority with reac_rt_prio_resolve() and raise the" >&2
	echo "      thread with reac_rt_thread_go(); see the ladder in reac_rt.h." >&2
	exit 1
fi

echo "rt-prio-single-door: OK (reac-pw's own src/ never sets a scheduling class; libreac-transport's reac_rt.c is the door)"
