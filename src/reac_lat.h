// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_lat — ProcessLatency smoothing for the reac:playback sink (task #152).
 *
 * The graph->wire delay a sample sees is the 12-sample staging accumulator plus
 * the pacer frame-ring depth. That depth SAWTOOTHS with each producer burst
 * (measured live ~38..113 frames = 9.5..28 ms), so reporting the raw
 * instantaneous depth as SPA_PARAM_ProcessLatency re-advertised the value on
 * essentially every 200 ms poll — pw_filter_update_params churn.
 *
 * ProcessLatency should be a STABLE contract, so this smooths the drain-observed
 * depth with an EMA (converging to the sawtooth MEAN, ~19 ms — the depth the ring
 * actually floats around, NOT the guard TARGET, which is only the drain-to level
 * under pathological drift and never reached in normal operation) and applies
 * hysteresis: re-advertise only when the smoothed latency moves by a material,
 * sustained step. Pure math, no PipeWire — the sink's 200 ms main-loop timer
 * drives it; the RT path is untouched. */
#ifndef REAC_LAT_H
#define REAC_LAT_H

#include <stdint.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

/* EMA smoothing factor applied per 200 ms depth poll (~2 s time constant). Small
 * enough that the aliased per-burst sawtooth ripples the EMA by well under the
 * material step below, so steady playback re-advertises once, not every poll. */
#define REAC_LAT_EMA_ALPHA    0.1f

/* Re-advertise only when the smoothed latency moves >= this from the last
 * advertised value — a sustained regime change (playback start/stop, a real
 * drift), never the per-burst ripple. */
#define REAC_LAT_MATERIAL_NS  3000000LL   /* 3 ms */

struct reac_lat {
	float   depth_ema;     /* smoothed ring depth in frames (<0 = uninitialised) */
	int64_t advertised_ns; /* last-advertised ProcessLatency (0 = none yet)       */
};

/* Reset to "nothing observed / advertised yet". */
void reac_lat_init(struct reac_lat *s);

/* Fold one drain-observed instantaneous ring depth (frames) into the EMA and
 * return the new smoothed depth. Seeds to the first sample. PURE. */
float reac_lat_ema_update(struct reac_lat *s, uint32_t depth_frames);

/* Smoothed latency in ns for a depth EMA at `sample_rate` Hz: staging (one REAC
 * frame) + the smoothed ring depth, each REAC_SAMPLES_PER_PKT samples. PURE. */
int64_t reac_lat_ns(float depth_ema, int sample_rate);

/* Advance the smoother by one poll: fold `depth_frames` into the EMA, compute the
 * smoothed latency into *out_ns, and return 1 iff it moved >= REAC_LAT_MATERIAL_NS
 * from the last advertised value (or nothing has been advertised yet) — in which
 * case the new value becomes the baseline and the caller should re-advertise.
 * Returns 0 (and advertises nothing) when the smoothed latency is stable. PURE. */
int reac_lat_poll(struct reac_lat *s, uint32_t depth_frames, int sample_rate,
                  int64_t *out_ns);

#endif /* REAC_LAT_H */
