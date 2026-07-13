// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ProcessLatency smoother (task #152): the reac:playback graph->wire delay must
 * be advertised as a STABLE contract, not the raw ring depth — which sawtooths
 * ~38..113 frames (measured live) with every producer burst and so re-advertised
 * pw_filter_update_params on essentially every 200 ms poll. This asserts:
 *   1. the pure EMA / latency math;
 *   2. fed the live 38..113 sawtooth, the smoothed latency settles into a tight
 *      band around the sawtooth MEAN and, once settled, triggers ZERO further
 *      re-advertises (the whole point of the fix). */
#include "reac_lat.h"
#include <reac/reac.h>

#include <stdio.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	const int rate = 48000;

	/* 1. pure math. staging (1 frame) + depth, each REAC_SAMPLES_PER_PKT samples. */
	CHK(reac_lat_ns(0.0f, rate) == (int64_t)REAC_SAMPLES_PER_PKT * 1000000000LL / rate);
	/* depth 75 (the live sawtooth mean) -> (1+75)*12 = 912 samples = 19.0 ms. */
	CHK(reac_lat_ns(75.0f, rate) == (int64_t)(76 * 12) * 1000000000LL / rate);
	CHK(reac_lat_ns(75.0f, rate) == 19000000LL);
	CHK(reac_lat_ns(10.0f, 0) == 0);              /* guard: no rate */

	/* EMA seeds to the first sample, then converges toward the input mean. */
	{
		struct reac_lat s; reac_lat_init(&s);
		CHK(reac_lat_ema_update(&s, 40) == 40.0f); /* seed, no lag-in */
		for (int i = 0; i < 500; i++)
			reac_lat_ema_update(&s, 75);
		float e = reac_lat_ema_update(&s, 75);
		CHK(e > 74.9f && e < 75.1f);               /* converged to the mean */
	}

	/* 2. the live 38..113 sawtooth (triangle, mean ~75) fed at 200 ms/poll. */
	static const uint32_t saw[8] = { 38, 56, 75, 94, 113, 94, 75, 56 };

	struct reac_lat s; reac_lat_init(&s);
	int64_t ns = 0;

	/* Warmup: the EMA climbs from the first sample toward the mean, crossing a few
	 * 3 ms steps — a handful of re-advertises, NOT one per poll. */
	int warmup_adv = 0;
	for (int i = 0; i < 200; i++)
		if (reac_lat_poll(&s, saw[i % 8], rate, &ns))
			warmup_adv++;
	CHK(warmup_adv >= 1 && warmup_adv <= 6);       /* bounded, not ~200 */

	/* Settled: another 1000 polls of the SAME sawtooth must trigger ZERO
	 * re-advertises — the EMA ripple stays under the 3 ms material step. This is
	 * the churn fix: steady playback advertises once, then goes quiet. */
	int settled_adv = 0;
	int64_t lo = INT64_MAX, hi = INT64_MIN;
	for (int i = 0; i < 1000; i++) {
		if (reac_lat_poll(&s, saw[i % 8], rate, &ns))
			settled_adv++;
		if (ns < lo) lo = ns;
		if (ns > hi) hi = ns;
	}
	CHK(settled_adv == 0);

	/* The smoothed latency tracks the ~19 ms mean-depth contract and stays in a
	 * tight band (well inside the raw 9.75..28.5 ms sawtooth range). */
	int64_t mean_ns = reac_lat_ns(75.0f, rate);    /* 19 ms */
	CHK(lo > mean_ns - 3000000LL && hi < mean_ns + 3000000LL);
	CHK(hi - lo < 3000000LL);                      /* peak-to-peak ripple < 3 ms */

	/* A genuine sustained regime change (playback stops -> ring drains to empty)
	 * DOES re-advertise, down toward the staging-only floor. */
	int drain_adv = 0;
	for (int i = 0; i < 200; i++)
		if (reac_lat_poll(&s, 0, rate, &ns))
			drain_adv++;
	CHK(drain_adv >= 1);
	CHK(ns < 1000000LL);                           /* settled near staging-only (<1 ms) */

	printf("OK: reac_lat EMA smoothing — warmup adv=%d, settled adv=0 over a "
	       "38..113 sawtooth (band %.2f..%.2f ms), drain adv=%d\n",
	       warmup_adv, (double)lo / 1e6, (double)hi / 1e6, drain_adv);
	return 0;
}
