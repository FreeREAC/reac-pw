// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_sink_format.h"

#include <stdlib.h>

#include <reac/reac.h>   /* REAC_MAX_CHANNELS */

#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

/* PROBE (REACPW_RATE_CHOICE=1): declare the rate as a CHOICE at connect —
 * default `rate`, alternatives the closed REAC set — instead of one fixed value.
 *
 * The point is to find out whether an already-negotiated stream can be moved to
 * another rate INSIDE its declared choice without its ports being destroyed. The
 * adapter refuses a Format change on a started node
 * (audioconvert/audioadapter.c: `if (this->started) return -EIO`), so this pairs
 * with REACPW_RATE_VIA_ACTIVE, which stops the node without disconnecting it.
 *
 * Success is FIVE things, not one: the rate moves, the node id survives, the PORT
 * ids survive, the consumer's links survive, and the RT thread never dies. Only
 * the last three decide whether players on a fed segment keep playing. */
static int rate_choice(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("REACPW_RATE_CHOICE");
		cached = (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' ||
		                v[0] == 't' || v[0] == 'T')) ? 1 : 0;
	}
	return cached;
}

static const struct spa_pod *build_rate_choice_format(struct spa_pod_builder *b,
                                                      int channels, int rate)
{
	static const int REAC_RATES[] = { 44100, 48000, 96000 };
	struct spa_pod_frame f[2];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
		SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
		SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
		SPA_FORMAT_AUDIO_channels, SPA_POD_Int(channels),
		0);

	/* rate: Enum choice, default first (SPA's convention), then the alternatives. */
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
	spa_pod_builder_int(b, rate);
	for (size_t i = 0; i < sizeof REAC_RATES / sizeof REAC_RATES[0]; i++)
		spa_pod_builder_int(b, REAC_RATES[i]);
	spa_pod_builder_pop(b, &f[1]);

	if (channels > 0) {
		uint32_t pos[REAC_MAX_CHANNELS];
		for (int c = 0; c < channels; c++)
			pos[c] = (uint32_t)(SPA_AUDIO_CHANNEL_AUX0 + c);
		spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_position, 0);
		spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Id, (uint32_t)channels, pos);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

const struct spa_pod *reac_sink_format_build(struct spa_pod_builder *b,
                                             int channels, int rate)
{
	if (channels < 0)
		channels = 0;
	if (channels > REAC_MAX_CHANNELS)
		channels = REAC_MAX_CHANNELS;

	if (rate_choice())
		return build_rate_choice_format(b, channels, rate);

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
