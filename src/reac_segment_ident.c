// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_segment_ident — see the header for what a segment publishes about itself
// and why a slave's answer set is derived rather than asserted.

#include "reac_segment_ident.h"
#include <reac/reac_arbitration.h>   /* the ONE vocabulary these strings come from */
#include <reac/reac.h>          /* the geometry vocabulary these widths are read in */

#include <stdio.h>
#include <string.h>

const char *reac_segment_name(const char *inst)
{
	return (inst && *inst) ? inst : REAC_SEGMENT_NAME_DEFAULT;
}

void reac_segment_heard_init(struct reac_segment_heard *h, uint64_t frames_ok)
{
	if (!h)
		return;
	h->last_frames = frames_ok;
	h->quiet_ticks = 0;
	h->heard = 0;
}

int reac_segment_heard_step(struct reac_segment_heard *h, uint64_t frames_ok,
                            int quiet_limit)
{
	if (!h)
		return 0;
	if (frames_ok != h->last_frames) {
		/* A frame arrived since the last step: heard, now, on this evidence. */
		h->last_frames = frames_ok;
		h->quiet_ticks = 0;
		h->heard = 1;
		return h->heard;
	}
	/* Nothing new. Age the claim rather than keep it: the counter is cumulative
	 * and would otherwise vouch for a desk that has been unplugged since. */
	if (h->quiet_ticks < quiet_limit)
		h->quiet_ticks++;
	if (h->quiet_ticks >= quiet_limit)
		h->heard = 0;
	return h->heard;
}

/* The two composers below share everything except WHO decided what, so the
 * formatting lives here once: a second copy of it is a second vocabulary. */
static void answer_fill(struct reac_segment_answer *out,
                        enum reac_segment_master state,
                        enum reac_rival_kind rival,
                        const char *refusal,
                        enum reac_pace_source pace,
                        uint64_t mac48, int rate_hz)
{
	memset(out, 0, sizeof *out);
	snprintf(out->master_state, sizeof out->master_state, "%s",
	         reac_segment_master_name(state));
	snprintf(out->rival_kind, sizeof out->rival_kind, "%s",
	         reac_rival_kind_name(rival));
	snprintf(out->refusal, sizeof out->refusal, "%s", refusal);
	snprintf(out->pace_source, sizeof out->pace_source, "%s",
	         reac_pace_source_name(pace));
	/* Not a master, so the mid-flight master-vs-master dispute this flag reports
	 * cannot arise here. Published rather than omitted because "0" is a fact. */
	snprintf(out->conflict, sizeof out->conflict, "0");

	if (mac48 != 0) {
		uint8_t mac[6];
		reac_mac48_unpack(mac48, mac);
		snprintf(out->master_mac, sizeof out->master_mac,
		         "%02x:%02x:%02x:%02x:%02x:%02x",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		snprintf(out->master_mac, sizeof out->master_mac, "none");
	}

	snprintf(out->rate, sizeof out->rate, "%d", rate_hz > 0 ? rate_hz : 0);
}

void reac_segment_answer_slave(struct reac_segment_answer *out, int heard,
                               uint64_t master_mac48, int rate_hz,
                               unsigned wire_channels)
{
	if (!out)
		return;
	/* The width the RX gate is accepting IS the geometry — 40 for a desk's
	 * downstream, the box's own for a stagebox on M we joined — so ask the
	 * master's own classifier what that width means rather than inventing a
	 * second rule. It used to be hard-coded to REAC_MAX_CHANNELS, which was true
	 * only while a desk was the only master a slave could ever be joined to. */
	enum reac_rival_kind rival = heard
		? reac_rival_kind_from_channels(wire_channels)
		: REAC_RIVAL_NONE;
	/* WE JOINED, SO WE REFUSED NOTHING — a fact about us, not about the peer. A
	 * box carries a refusal code for the one case that still refuses (a wire
	 * pinned master), and that answer is composed by reac_segment_answer_refused;
	 * publishing the code here would report this segment as declining the very
	 * master it is following. */
	answer_fill(out, heard ? REAC_SEGMENT_FOREIGN : REAC_SEGMENT_NONE, rival,
	            reac_rival_refusal(REAC_RIVAL_NONE),
	            heard ? REAC_PACE_FOREIGN_MASTER : REAC_PACE_FREE_RUN,
	            master_mac48, rate_hz);
}

void reac_segment_answer_refused(struct reac_segment_answer *out,
                                 unsigned rival_channels, uint64_t rival_mac48,
                                 int rate_hz)
{
	if (!out)
		return;
	enum reac_rival_kind rival = reac_rival_kind_from_channels(rival_channels);
	/* FOREIGN is not a guess here: this answer exists because arbitration found an
	 * unambiguous other master on the wire. The pace is that master's for the same
	 * reason — it is the only thing transmitting, and we are not. */
	answer_fill(out, REAC_SEGMENT_FOREIGN, rival, reac_rival_refusal(rival),
	            REAC_PACE_FOREIGN_MASTER, rival_mac48, rate_hz);
}
