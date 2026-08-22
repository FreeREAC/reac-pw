// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_arbitration — see reac_arbitration.h for what this decides (nothing) and why.

#include "reac_arbitration.h"

#include <string.h>

const char *reac_segment_master_name(enum reac_segment_master s)
{
	switch (s) {
	case REAC_SEGMENT_US:      return "us";
	case REAC_SEGMENT_FOREIGN: return "foreign";
	case REAC_SEGMENT_NONE:
	default:                  return "none";
	}
}

const char *reac_pace_source_name(enum reac_pace_source p)
{
	switch (p) {
	case REAC_PACE_FOREIGN_MASTER: return "foreign-master";
	case REAC_PACE_PHC:            return "phc";
	case REAC_PACE_GRAPH_REF:      return "graph-ref";
	case REAC_PACE_BOX_SLOPE:      return "box-slope";
	case REAC_PACE_FREE_RUN:
	default:                       return "free-run";
	}
}

/**
 * The newest LIVE foreign master in the table, or NULL.
 *
 * Only `REAC_DISCO_ROLE_MASTER` counts. An UNKNOWN-role sighting is deliberately not evidence
 * either way — it does not become a master, and it does not contradict one — because the
 * catch-all bucket is exactly where a box gets misfiled, and acting on that misfile costs the
 * segment.
 *
 * Newest wins when two are somehow present: two foreign masters is already a broken segment,
 * and naming the one still talking is more use to an operator than naming whichever was
 * recorded first.
 */
static const struct reac_disco_entry *foreign_master(const struct reac_disco_table *t,
                                                     const uint8_t our_mac[6],
                                                     uint64_t now_ns)
{
	const struct reac_disco_entry *best = NULL;
	for (int i = 0; i < t->n; i++) {
		const struct reac_disco_entry *e = &t->e[i];
		if (e->role != REAC_DISCO_ROLE_MASTER)
			continue;
		if (our_mac && memcmp(e->mac, our_mac, 6) == 0)
			continue;                      /* our own echo is not a rival */
		/* A master heard once and gone is not a master. The table's own staleness bar is
		 * what decides that, so an unplugged desk stops being reported. */
		if (now_ns > e->last_seen_ns && now_ns - e->last_seen_ns > REAC_DISCO_STALE_NS)
			continue;
		if (!best || e->last_seen_ns > best->last_seen_ns)
			best = e;
	}
	return best;
}

void reac_arbitrate(const struct reac_disco_table *table,
                    const uint8_t our_mac[6],
                    enum reac_master_state fsm,
                    enum reac_pace_source own_pace,
                    uint64_t now_ns,
                    struct reac_arbitration *out)
{
	memset(out, 0, sizeof *out);
	out->pace = own_pace;

	/* GRANTING is established-in-progress: a box has joined and we are mid-handshake, so the
	 * segment is already ours. Treating it as "not established" would report the wire as
	 * foreign for the seconds a rival happened to be heard during a grant burst. */
	const int established = (fsm == REAC_M_ESTABLISHED || fsm == REAC_M_GRANTING);
	const int probing = (fsm == REAC_M_PROBING);

	const struct reac_disco_entry *rival = table ? foreign_master(table, our_mac, now_ns) : NULL;

	if (established) {
		/* WE drive: we are established with a box, whatever else is on the wire. Saying
		 * `foreign` here would tell a surface the desk had taken over when it has not —
		 * the audio is still ours. A rival that appears now is the mid-flight conflict,
		 * reported separately and acted on by nobody until Q1 is answered. */
		out->state = REAC_SEGMENT_US;
		out->conflict = rival != NULL;
		if (our_mac) {
			memcpy(out->mac, our_mac, 6);
			out->have_mac = 1;
		}
		return;
	}

	if (rival) {
		/* Not established, and something else unambiguously masters this wire. One master
		 * per segment is REAC law and we are not the second one. */
		out->state = REAC_SEGMENT_FOREIGN;
		memcpy(out->mac, rival->mac, 6);
		out->have_mac = 1;
		/* The foreign master times the stream; whatever WE would have disciplined to is
		 * not what the wire is running on. */
		out->pace = REAC_PACE_FOREIGN_MASTER;
		return;
	}

	if (probing) {
		/* Probing UNOPPOSED — no rival was found above — so the segment is ours to take. */
		out->state = REAC_SEGMENT_US;
		if (our_mac) {
			memcpy(out->mac, our_mac, 6);
			out->have_mac = 1;
		}
		return;
	}

	/* Nothing established, nothing probing, no master evidence: the wire is silent. Reported
	 * as NONE with no MAC, which is honestly different from "we drive". */
	out->state = REAC_SEGMENT_NONE;
}
