// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_watch — see reac_watch.h for what a served segment's fresh verdict means.

#include "reac_watch.h"
#include "reac_disco.h"

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
	return served == REAC_HUNT_MASTER && !pinned;
}

enum reac_watch_act reac_watch_decide(const struct reac_watch_in *in)
{
	if (!in)
		return REAC_WATCH_STAND;

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
	 * already — not from a hunt that never ran. */
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
