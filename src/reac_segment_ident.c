// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_segment_ident — see the header for what a segment publishes about itself
// and why a slave's answer set is derived rather than asserted.

#include "reac_segment_ident.h"
#include "reac_arbitration.h"   /* the ONE vocabulary these strings come from */
#include <reac/reac.h>          /* REAC_MAX_CHANNELS — the master downstream width */

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

void reac_segment_answer_slave(struct reac_segment_answer *out, int heard,
                               uint64_t master_mac48, int rate_hz)
{
	if (!out)
		return;
	memset(out, 0, sizeof *out);

	enum reac_segment_master state = heard ? REAC_SEGMENT_FOREIGN : REAC_SEGMENT_NONE;
	/* The gate that let those frames through accepts the 40-channel master
	 * downstream and nothing else, so the geometry is known without a second
	 * classification — ask the master's own classifier what that width means. */
	enum reac_rival_kind rival = heard
		? reac_rival_kind_from_channels(REAC_MAX_CHANNELS)
		: REAC_RIVAL_NONE;
	enum reac_pace_source pace = heard ? REAC_PACE_FOREIGN_MASTER : REAC_PACE_FREE_RUN;

	snprintf(out->master_state, sizeof out->master_state, "%s",
	         reac_segment_master_name(state));
	snprintf(out->rival_kind, sizeof out->rival_kind, "%s",
	         reac_rival_kind_name(rival));
	snprintf(out->refusal, sizeof out->refusal, "%s", reac_rival_refusal(rival));
	snprintf(out->pace_source, sizeof out->pace_source, "%s",
	         reac_pace_source_name(pace));
	/* Not a master, so the mid-flight master-vs-master dispute this flag reports
	 * cannot arise here. Published rather than omitted because "0" is a fact. */
	snprintf(out->conflict, sizeof out->conflict, "0");

	if (master_mac48 != 0) {
		uint8_t mac[6];
		reac_mac48_unpack(master_mac48, mac);
		snprintf(out->master_mac, sizeof out->master_mac,
		         "%02x:%02x:%02x:%02x:%02x:%02x",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		snprintf(out->master_mac, sizeof out->master_mac, "none");
	}

	snprintf(out->rate, sizeof out->rate, "%d", rate_hz > 0 ? rate_hz : 0);
}
