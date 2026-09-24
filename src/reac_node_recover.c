// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_node_recover — see reac_node_recover.h for the ladder and why it is bounded.

#include "reac_node_recover.h"

#include <string.h>

void reac_node_recover_init(struct reac_node_recover *r)
{
	if (r)
		memset(r, 0, sizeof *r);
}

int reac_node_recover_window(const struct reac_node_recover *r)
{
	if (!r)
		return REAC_RECOVER_GRACE_TICKS;
	int shift = r->attempts < REAC_RECOVER_MAX_SHIFT ? r->attempts : REAC_RECOVER_MAX_SHIFT;
	return REAC_RECOVER_GRACE_TICKS << shift;
}

int reac_node_recover_spent(const struct reac_node_recover *r)
{
	if (!r || r->attempts <= 0)
		return REAC_RECOVER_GRACE_TICKS;
	int prev = r->attempts - 1;
	int shift = prev < REAC_RECOVER_MAX_SHIFT ? prev : REAC_RECOVER_MAX_SHIFT;
	return REAC_RECOVER_GRACE_TICKS << shift;
}

enum reac_node_recover_act reac_node_recover_step(struct reac_node_recover *r, int on_graph)
{
	if (!r)
		return REAC_RECOVER_WAIT;

	if (on_graph) {
		/* Back on the graph — including after a give-up. The ladder is about one
		 * absence, not a verdict on the segment: a box re-plugged or a session
		 * manager restarted deserves the full budget again. */
		reac_node_recover_init(r);
		return REAC_RECOVER_WAIT;
	}

	if (r->gave_up)
		return REAC_RECOVER_WAIT;   /* said once; a repeat is noise, not information */

	if (++r->absent < reac_node_recover_window(r))
		return REAC_RECOVER_WAIT;   /* still inside this attempt's window */

	r->absent = 0;
	if (r->attempts >= REAC_RECOVER_MAX_ATTEMPTS) {
		r->gave_up = 1;
		return REAC_RECOVER_GIVE_UP;
	}
	r->attempts++;
	return REAC_RECOVER_REBUILD;
}

struct reac_node_pair_verdict reac_node_recover_step_pair(struct reac_node_recover *r,
                                                          int src_on_graph, const char *src_why,
                                                          int sink_on_graph, const char *sink_why)
{
	struct reac_node_pair_verdict v;
	v.src_gone  = !src_on_graph;
	v.sink_gone = !sink_on_graph;
	/* THE MISSING SIDE'S OWN REASON. Both gone is the server-went-away shape and the
	 * two reasons agree; quote the capture side's, as the line always has. */
	v.why = v.src_gone ? src_why : v.sink_gone ? sink_why : src_why;
	/* The ladder counts the SEGMENT's absence: whole only when both sides are there. */
	v.act = reac_node_recover_step(r, src_on_graph && sink_on_graph);
	return v;
}

const char *reac_node_pair_name(const struct reac_node_pair_verdict *v)
{
	if (v && v->src_gone && !v->sink_gone)
		return "reac-capture";
	if (v && v->sink_gone && !v->src_gone)
		return "reac-playback";
	return "reac-capture and reac-playback";
}
