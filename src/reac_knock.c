// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_knock — see reac_knock.h for the defect, the safety argument and every number.

#include "reac_knock.h"

#include <string.h>

/* The jitter's generator: a plain 32-bit LCG (Numerical Recipes constants). It needs to
 * break a tie between two hosts, not to be unpredictable, and being seeded from the MAC
 * makes one host's knock schedule reproducible in a capture. */
static uint32_t next_rand(struct reac_knock *k)
{
	k->rng = k->rng * 1664525u + 1013904223u;
	return k->rng;
}

static uint64_t jitter(struct reac_knock *k)
{
	return (uint64_t)(next_rand(k) % (uint32_t)REAC_KNOCK_JITTER_NS);
}

void reac_knock_init(struct reac_knock *k, const uint8_t our_mac[6], uint64_t now_ns)
{
	memset(k, 0, sizeof *k);
	k->state = REAC_KNOCK_LISTENING;
	k->opened_ns = now_ns;
	/* THE FIRST KNOCK IS DUE WHEN THE MASTERLESS OBSERVATION CLOSES, not before: a wire
	 * we have not yet watched for REAC_KNOCK_LISTEN_NS has not been proven masterless,
	 * and the whole safety argument is that we never transmit onto one that has not. */
	k->next_ns = now_ns + REAC_KNOCK_LISTEN_NS;
	uint32_t seed = 0x9e3779b9u;
	if (our_mac)
		for (int i = 0; i < 6; i++)
			seed = seed * 31u + our_mac[i];
	k->rng = seed | 1u;
}

void reac_knock_heard(struct reac_knock *k)
{
	/* LISTENING and KNOCKING both end here, and they end the same way: the wire has
	 * something on it, so the hunt has evidence and this module's job is done. */
	k->state = REAC_KNOCK_STOPPED;
}

enum reac_knock_act reac_knock_step(struct reac_knock *k, uint64_t now_ns)
{
	switch (k->state) {
	case REAC_KNOCK_STOPPED:
		/* The END is reported exactly once, on the first step after the frame that
		 * stopped us, and only if we had actually started: a wire that answered inside
		 * its listening window never announced anything, so it has nothing to stop and
		 * says nothing. */
		if (k->end_said)
			return REAC_KNOCK_ACT_NONE;
		k->end_said = 1;
		return k->sent > 0 ? REAC_KNOCK_ACT_END : REAC_KNOCK_ACT_NONE;

	case REAC_KNOCK_LISTENING:
		if (now_ns < k->next_ns)
			return REAC_KNOCK_ACT_NONE;
		/* The observation closed on total silence. A master fills every slot, so this
		 * wire has no master on it — knock. */
		k->state = REAC_KNOCK_KNOCKING;
		k->sent++;
		k->next_ns = now_ns + REAC_KNOCK_PERIOD_NS + jitter(k);
		return REAC_KNOCK_ACT_BEGIN;

	case REAC_KNOCK_KNOCKING:
	default:
		if (now_ns < k->next_ns)
			return REAC_KNOCK_ACT_NONE;
		k->sent++;
		/* The next knock is scheduled from NOW, not from the due time: a poll that ran
		 * late must not make the following knocks come in a burst to catch up. There is
		 * nothing to catch up — a knock is a question, not a cadence. */
		k->next_ns = now_ns + REAC_KNOCK_PERIOD_NS + jitter(k);
		return REAC_KNOCK_ACT_SEND;
	}
}

const char *reac_knock_stop_reason(const struct reac_knock *k)
{
	(void)k;
	return "REAC heard";
}
