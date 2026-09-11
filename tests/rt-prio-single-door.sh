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
# (docs/design/specs/2026-09-11-reac-transport-library.md) — the door is no longer under
# this repo's src/ at all. The invariant this repo can still enforce on its own is the half
# that matters most HERE: reac-pw's OWN src/ never spells SCHED_FIFO a second way. The
# positive control (proving the sweep can see a real door, not just its absence) needs a
# sibling libreac checkout's transport/src, passed as $2; without it the check SKIPS
# (exit 77) rather than either a blind pass or a fail for a door it structurally cannot see.
#
# So: sched_setscheduler / pthread_setschedparam / pthread_attr_setschedparam appear
# nowhere under this repo's src/.
set -o pipefail
src="${1:?usage: rt-prio-single-door.sh SRCDIR [TRANSPORT_SRCDIR]}/src"
transport_src="${2:-}"
# The CALL, not the word: a doc comment naming the syscall is not a door.
pattern='(sched_setscheduler|pthread_setschedparam|pthread_attr_setschedparam)[[:space:]]*\('

# Positive control: the sweep must be able to detect presence, proven against the door's
# new home when we have one to look at.
if [ -n "$transport_src" ]; then
	door="$transport_src/reac_rt.c"
	if [ ! -f "$door" ] || ! grep -Eq "$pattern" "$door"; then
		echo "FAIL: the sweep found no scheduling call in $door — either the door" >&2
		echo "      moved again or this check is broken; it cannot report absence either way." >&2
		exit 1
	fi
else
	echo "SKIP: no TRANSPORT_SRCDIR given — cannot prove the sweep can detect a real door" >&2
	echo "      (see -Dlibreac_transport_srcdir). Still checking this repo's src/ has none." >&2
fi

offenders=$(grep -rlE "$pattern" "$src" --include='*.c' --include='*.h' || true)
if [ -n "$offenders" ]; then
	echo "FAIL: SCHED_FIFO is set in reac-pw's own src/, outside libreac-transport's reac_rt.c:" >&2
	echo "$offenders" >&2
	echo "      Resolve the priority with reac_rt_prio_resolve() and raise the" >&2
	echo "      thread with reac_rt_thread_go(); see the ladder in reac_rt.h." >&2
	exit 1
fi

if [ -z "$transport_src" ]; then
	exit 77
fi
echo "rt-prio-single-door: OK (reac_rt.c, in libreac-transport, is the only door to SCHED_FIFO)"
