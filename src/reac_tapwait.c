// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_tapwait — see reac_tapwait.h for the two sockets, the nine minutes and the bar.

#include "reac_tapwait.h"

int reac_tapwait_binds(const struct reac_tapwait_in *in)
{
	if (!in || !in->tapped)
		return 0;
	/* THE TAP HAS SPOKEN. An untagged classification places this wire on the parent
	 * itself, and the sniffer's sightings are about a segment we may elect a role on. */
	if (in->untagged > 0)
		return 0;
	/* NOTHING WAS EVER HEARD HERE. There is no sighting to hold anything up: the wire is
	 * quiet, and a quiet wire is the masterless observation's case, not this one. */
	if (in->last_heard_ns == 0)
		return 0;
	/* A SIGHTING BINDS WHILE IT IS FRESH, and not one poll longer. Unsigned time
	 * subtracts in the right order or not at all (the wrap trap reac_watch_decide's
	 * dwell documents): a stamp in the future is treated as just-heard. */
	if (in->now_ns <= in->last_heard_ns)
		return 1;
	return in->now_ns - in->last_heard_ns < REAC_TAPWAIT_NS;
}
