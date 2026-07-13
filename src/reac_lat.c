// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_lat.h"

#include <reac/reac.h>   /* REAC_SAMPLES_PER_PKT */

void reac_lat_init(struct reac_lat *s)
{
	s->depth_ema = -1.0f;    /* uninitialised: the first update seeds the EMA */
	s->advertised_ns = 0;    /* nothing advertised yet: the first poll advertises */
}

float reac_lat_ema_update(struct reac_lat *s, uint32_t depth_frames)
{
	float sample = (float)depth_frames;
	if (s->depth_ema < 0.0f)
		s->depth_ema = sample;   /* seed on the first observation (no lag-in) */
	else
		s->depth_ema += REAC_LAT_EMA_ALPHA * (sample - s->depth_ema);
	return s->depth_ema;
}

int64_t reac_lat_ns(float depth_ema, int sample_rate)
{
	if (sample_rate <= 0)
		return 0;
	if (depth_ema < 0.0f)
		depth_ema = 0.0f;
	/* staging (one REAC frame) + the smoothed ring depth, each 12 samples. */
	double samples = (double)REAC_SAMPLES_PER_PKT * (1.0 + (double)depth_ema);
	return (int64_t)(samples * 1e9 / (double)sample_rate + 0.5);
}

int reac_lat_poll(struct reac_lat *s, uint32_t depth_frames, int sample_rate,
                  int64_t *out_ns)
{
	float ema = reac_lat_ema_update(s, depth_frames);
	int64_t ns = reac_lat_ns(ema, sample_rate);
	if (out_ns)
		*out_ns = ns;

	int64_t d = ns - s->advertised_ns;
	if (d < 0)
		d = -d;
	int material = (s->advertised_ns == 0) || (d >= REAC_LAT_MATERIAL_NS);
	if (material)
		s->advertised_ns = ns;   /* new baseline */
	return material;
}
