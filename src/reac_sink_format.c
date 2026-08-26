// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_sink_format.h"

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

int reac_sink_format_needs_update(int node_rate, int pacer_rate)
{
	if (pacer_rate <= 0)
		return 0;
	return node_rate != pacer_rate;
}
