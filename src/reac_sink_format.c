// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_sink_format.h"

#include <stdlib.h>

#include <reac/reac.h>   /* REAC_MAX_CHANNELS */

#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

const struct spa_pod *reac_sink_format_build(struct spa_pod_builder *b,
                                             int channels, int rate)
{
	if (channels < 0)
		channels = 0;
	if (channels > REAC_MAX_CHANNELS)
		channels = REAC_MAX_CHANNELS;

	struct spa_audio_info_raw finfo = {
		.format = SPA_AUDIO_FORMAT_F32P,
		.rate = (uint32_t)rate,
		.channels = (uint32_t)channels,
	};
	for (int c = 0; c < channels; c++)
		finfo.position[c] = (uint32_t)(SPA_AUDIO_CHANNEL_AUX0 + c);

	return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &finfo);
}

/* PROBE KNOB (default UNSET = today's behaviour, byte-identical): with
 * REACPW_FIXED_NODE_RATE=1 the node's FORMAT never follows the wire, so a pace
 * change never renegotiates it and the pw_stream is never disconnected.
 *
 * WHAT IT IS FOR. A rate change today REPLACES the PipeWire node: measured with
 * pw-mon on 2026-08-29, reac-playback.s1608 went id 416 -> 418 and reac-capture
 * 418 -> 380 across one change. The console's output link dies with the node, and
 * on the segment the console feeds (the PA return) the upstream pulse players
 * cork and never resume. This knob isolates that claim: if the format never
 * moves, the node must survive, and the players must never see anything.
 *
 * IT IS NOT A FIX AND MUST NOT BE SHIPPED ON. With the format pinned, the node
 * keeps delivering at its boot rate while the wire runs at another, so the
 * segment's audio is off by exactly that ratio — 2x at 48k under a 96k node.
 * The real design it probes is a node whose format is rate-INVARIANT with the
 * conversion done deliberately (PipeWire's resampler already does graph<->node;
 * the missing half is node<->wire), which is the open work. */
static int fixed_node_rate(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("REACPW_FIXED_NODE_RATE");
		cached = (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' ||
		                v[0] == 't' || v[0] == 'T')) ? 1 : 0;
	}
	return cached;
}

int reac_sink_format_needs_update(int node_rate, int pacer_rate)
{
	if (fixed_node_rate())
		return 0;   /* the format is pinned: never renegotiate, never reconnect */
	if (pacer_rate <= 0)
		return 0;
	return node_rate != pacer_rate;
}

int reac_sink_format_rate_after_attempt(int requested_hz, int prev_hz, int connect_ok)
{
	return connect_ok ? requested_hz : prev_hz;
}
