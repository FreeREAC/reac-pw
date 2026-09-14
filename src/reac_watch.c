// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_watch — see reac_watch.h for what a served segment's fresh verdict means.

#include "reac_watch.h"
#include <reac/reac_disco.h>

const char *reac_watch_act_name(enum reac_watch_act a)
{
	switch (a) {
	case REAC_WATCH_YIELD:    return "yield";
	case REAC_WATCH_RETAKE:   return "retake";
	case REAC_WATCH_UNREFUSE: return "unrefuse";
	case REAC_WATCH_STAND:
	default:                  return "stand";
	}
}

int reac_watch_keep(enum reac_hunt_verdict served, int pinned)
{
	if (served == REAC_HUNT_REFUSED)
		return 1;
	/* A VACANT DOOR is served on exactly this verdict and exists to be replaced the
	 * moment the wire says anything at all, so it keeps its sniffer. */
	if (served == REAC_HUNT_HUNTING)
		return 1;
	/* A SEGMENT WE JOINED STILL DOES NOT — see the header. The sniffer cannot answer
	 * "has my master left" while an enrolment is running (a box master's stream is
	 * mostly filler, which the peer lock refuses as a sighting), and answering it from
	 * here destroyed a live join. #97 is answered by hearing_reevaluate, from the
	 * segment's own decoded-frame latch. */
	return served == REAC_HUNT_MASTER && !pinned;
}

enum reac_watch_act reac_watch_decide(const struct reac_watch_in *in)
{
	if (!in)
		return REAC_WATCH_STAND;

	if (in->door && in->vacant) {
		/* A VACANT DOOR HAS NOTHING TO WAIT OUT. The refusal's dwell below asks "is
		 * the rival really gone", and this door was never about a rival: it went up
		 * because nothing had been heard. So the first verdict that decides the wire
		 * — a master, a desk, a rival worth refusing — replaces it at once, through
		 * the same drop-and-serve seam an unrefusal uses. */
		return in->verdict == REAC_HUNT_HUNTING ? REAC_WATCH_STAND : REAC_WATCH_UNREFUSE;
	}

	if (in->door) {
		/* The refusal stands while the rival is still mastering the wire. */
		if (in->verdict == REAC_HUNT_REFUSED)
			return REAC_WATCH_STAND;
		/* AN EMPTY TABLE IS NOT EVIDENCE THAT THE RIVAL LEFT, and unsigned time
		 * compares in the right order or not at all: a sniffer opened inside the
		 * same poll stamps a LATER clock than the poll's own, and the subtraction
		 * wrapped to ~584 years. */
		if (in->now_ns <= in->opened_ns ||
		    in->now_ns - in->opened_ns < REAC_DISCO_STALE_NS)
			return REAC_WATCH_STAND;
		return REAC_WATCH_UNREFUSE;
	}

	/* A PIN IS THE OPERATOR'S ANSWER ABOUT THIS WIRE and is never overturned here. The
	 * one thing a rival can do to a pinned segment is the 0.5.1 refusal, and that is
	 * decided by the segment's own engine, which classifies every frame on the wire
	 * already — not from a hunt that never ran. A pin that DEFERRED to a foreign master
	 * is taken up again by hearing_reevaluate, on the segment's own evidence (#97). */
	if (in->pinned)
		return REAC_WATCH_STAND;

	if (in->verdict == REAC_HUNT_SLAVE)
		return in->we_master ? REAC_WATCH_YIELD : REAC_WATCH_STAND;
	if (in->verdict == REAC_HUNT_MASTER)
		return in->we_master ? REAC_WATCH_STAND : REAC_WATCH_RETAKE;

	/* HUNTING is a wire that has said nothing yet, and REFUSED on a segment with an
	 * engine is a rival nobody has captured: neither hands a role over. */
	return REAC_WATCH_STAND;
}
