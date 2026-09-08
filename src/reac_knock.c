// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_knock — see reac_knock.h for the defect, the rig correction and the safety argument.

#include "reac_knock.h"

#include <string.h>

void reac_knock_init(struct reac_knock *k, uint64_t now_ns)
{
	memset(k, 0, sizeof *k);
	k->state = REAC_KNOCK_LISTENING;
	k->opened_ns = now_ns;
	/* THE LICENCE FALLS DUE WHEN THE MASTERLESS OBSERVATION CLOSES, not before: a wire
	 * we have not yet watched for REAC_KNOCK_LISTEN_NS has not been proven masterless,
	 * and the whole safety argument is that we never drive one that has not. */
	k->due_ns = now_ns + REAC_KNOCK_LISTEN_NS;
}

void reac_knock_heard(struct reac_knock *k)
{
	/* Only the wire that is still being OBSERVED can be cancelled. Once the licence has
	 * been granted the segment exists, and revoking it is a drop-and-re-serve that this
	 * pure module has no business performing — main.c watches the retained sniffer and
	 * yields the segment there. */
	if (k->state == REAC_KNOCK_LISTENING)
		k->state = REAC_KNOCK_CANCELLED;
}

enum reac_knock_act reac_knock_step(struct reac_knock *k, uint64_t now_ns)
{
	if (k->state != REAC_KNOCK_LISTENING)
		return REAC_KNOCK_ACT_NONE;
	if (now_ns < k->due_ns)
		return REAC_KNOCK_ACT_NONE;
	/* The observation closed on total silence. A master fills every audio slot, so this
	 * wire has no master on it — take it. */
	k->state = REAC_KNOCK_PROVEN;
	k->granted = 1;
	return REAC_KNOCK_ACT_DRIVE;
}
