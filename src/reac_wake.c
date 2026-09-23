// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_wake — see reac_wake.h for the live failure, the firmware law behind it and the
// safety argument. This file is the ladder; docs/design/specs/
// 2026-09-16-a-dropped-box-wakes-on-a-phy-edge.md §3-§5 is what it instantiates.

#include "reac_wake.h"

#include <string.h>

void reac_wake_init(struct reac_wake *w, uint64_t now_ns)
{
	memset(w, 0, sizeof *w);
	w->opened_ns = now_ns;
}

void reac_wake_reopen(struct reac_wake *w, uint64_t now_ns)
{
	w->opened_ns = now_ns;
	w->refusal = REAC_WAKE_OK;
	w->said = REAC_WAKE_OK;
}

int reac_wake_refusal_to_say(struct reac_wake *w)
{
	if (w->refusal == REAC_WAKE_OK || w->refusal == w->said)
		return 0;
	w->said = w->refusal;
	return 1;
}

enum reac_wake_act reac_wake_step(struct reac_wake *w, uint64_t now_ns,
                                  const struct reac_wake_obs *o)
{
	/* THE LADDER IS READ IN THIS ORDER ON PURPOSE: cheapest fact first, and the one
	 * that would be most wrong to act on before the one that is merely early. A wire
	 * with no carrier is not a box to wake; a carrier we could not read is not
	 * evidence of anything; a box that is talking is not silent. Only then does the
	 * question "have we played the cheap rung yet" arise at all. */
	if (!o->probing) {
		/* Granting or established: an edge here tears down the thing that works. */
		w->refusal = REAC_WAKE_NOT_PROBING;
		return REAC_WAKE_ACT_NONE;
	}
	if (o->carrier == 0) {
		w->refusal = REAC_WAKE_NO_CARRIER;
		return REAC_WAKE_ACT_NONE;
	}
	if (o->carrier < 0) {
		/* -1 is UNKNOWN and says nothing. An unreadable probe is never evidence
		 * about a link, and this one would spend an edge on the strength of it. */
		w->refusal = REAC_WAKE_CARRIER_UNKNOWN;
		return REAC_WAKE_ACT_NONE;
	}
	if (o->rx_box_frames > 0) {
		/* The far end is alive. Whatever is wrong, a PHY edge is not the remedy and
		 * the frames already arriving are the evidence against it. */
		w->refusal = REAC_WAKE_BOX_IS_TALKING;
		return REAC_WAKE_ACT_NONE;
	}
	if (o->tx_frames == 0) {
		/* NOTHING OF OURS IS REACHING THE WIRE. A box cannot have ignored a push it
		 * never received, and a PHY edge would only make it flood at a master that
		 * still cannot answer. The fault is on this side of the socket — the qdisc,
		 * the backend, the capability — and the edge is refused until a frame leaves. */
		w->refusal = REAC_WAKE_NOTHING_SENT;
		return REAC_WAKE_ACT_NONE;
	}
	if (o->siblings_served) {
		/* A bounce takes the device's VLAN children down with it. We never break a
		 * segment that is serving to wake one that is not. */
		w->refusal = REAC_WAKE_SIBLING_SERVED;
		return REAC_WAKE_ACT_NONE;
	}
	if (w->bounces >= REAC_WAKE_MAX_BOUNCES) {
		w->refusal = REAC_WAKE_SPENT;
		if (w->spent_said)
			return REAC_WAKE_ACT_NONE;
		w->spent_said = 1;
		return REAC_WAKE_ACT_EXHAUSTED;
	}
	if (w->last_bounce_ns && now_ns - w->last_bounce_ns < REAC_WAKE_SETTLE_NS) {
		/* The box owes us a flood, a cold-connect and our own grant dwell. */
		w->refusal = REAC_WAKE_SETTLING;
		return REAC_WAKE_ACT_NONE;
	}
	/* THE CHEAP RUNG, AND IT IS COUNTED IN TRANSFERS. The clock is only the floor
	 * underneath: a master whose push never COMPLETES has not played this rung at
	 * all, and an interrupted transfer is measured to produce nothing. Both must be
	 * satisfied, and the push count is the one that can refuse forever. */
	if (o->scene_pushes < REAC_WAKE_MIN_PUSHES ||
	    now_ns - w->opened_ns < REAC_WAKE_GRACE_NS) {
		w->refusal = REAC_WAKE_PUSH_NOT_PROVEN;
		return REAC_WAKE_ACT_NONE;
	}
	/* Everything cheap has been tried and the wire answered none of it. Make the box
	 * the one event its firmware leaves DROP on. */
	w->refusal = REAC_WAKE_OK;
	w->bounces++;
	w->last_bounce_ns = now_ns;
	return REAC_WAKE_ACT_BOUNCE;
}

const char *reac_wake_refusal_text(enum reac_wake_refusal r)
{
	switch (r) {
	case REAC_WAKE_OK:              return "nothing refused it";
	case REAC_WAKE_NOT_PROBING:     return "the master is not probing";
	case REAC_WAKE_NO_CARRIER:      return "there is no carrier to break";
	case REAC_WAKE_CARRIER_UNKNOWN: return "the carrier could not be read";
	case REAC_WAKE_BOX_IS_TALKING:  return "a box is transmitting";
	case REAC_WAKE_NOTHING_SENT:    return "our own frames are not leaving the host — "
	                                       "every send has failed, so no push has reached "
	                                       "the wire";
	case REAC_WAKE_PUSH_NOT_PROVEN: return "our own scene push has not completed yet";
	case REAC_WAKE_SETTLING:        return "the last edge is still settling";
	case REAC_WAKE_SIBLING_SERVED:  return "other segments are served over this device";
	case REAC_WAKE_SPENT:           return "the wake ladder is spent";
	}
	return "unknown";
}
