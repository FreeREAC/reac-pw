// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for the output-gain staging kernel reac_gain_ramp_block — the pure
 * function the reac:playback RT path calls to apply SPA_PROP volume/mute to the
 * box outputs (see reac_gain.h for the linear-scale rationale).
 *
 * Pins the contract the sink relies on:
 *   (a) UNITY is a true no-op multiply — a passthrough sink stays bit-exact;
 *   (b) a target is TRACKED and reached within the expected sample count, then
 *       held exactly (no drift, no overshoot), monotone the whole way;
 *   (c) MUTE (target 0) ramps down to silence and stays there;
 *   (d) two channels ramp INDEPENDENTLY — one channel's state never bleeds into
 *       another (the sink keeps a separate cur/target per channel);
 *   (e) a boost target (>1) ramps up and amplifies (over-amplification headroom);
 *   (f) a non-positive step snaps straight to the target (the no-ramp path).
 */
#include "reac_gain.h"

#include <stdio.h>
#include <math.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

/* step = 1/64: a clean binary fraction (0.015625) so ramp arithmetic is exact
 * and the landing sample is deterministic (no FP slack needed). */
#define STEP (1.0f / 64.0f)

int main(void)
{
	/* ---- (a) unity is a no-op: arbitrary samples survive a 1.0->1.0 ramp. */
	float buf[64];
	for (int i = 0; i < 64; i++)
		buf[i] = -0.9f + 0.03f * (float)i;   /* a spread of distinct values */
	float ret = reac_gain_ramp_block(buf, 64, 1.0f, 1.0f, STEP);
	CHK(ret == 1.0f);
	for (int i = 0; i < 64; i++)
		CHK(buf[i] == -0.9f + 0.03f * (float)i);   /* untouched, bit-exact */

	/* ---- (b) target tracking: 1.0 -> 0.5 with input held at 1.0 so buf[i] is
	 * exactly the gain applied at sample i. Reaches 0.5 at sample 32 (0.5 / step)
	 * and holds; strictly decreasing until it lands. */
	for (int i = 0; i < 64; i++) buf[i] = 1.0f;
	ret = reac_gain_ramp_block(buf, 64, 1.0f, 0.5f, STEP);
	CHK(ret == 0.5f);
	CHK(buf[31] == 0.5f);           /* lands exactly on the 32nd sample */
	CHK(buf[32] == 0.5f);           /* and holds — no overshoot below target */
	CHK(buf[63] == 0.5f);
	for (int i = 1; i < 31; i++)
		CHK(buf[i] < buf[i - 1]);   /* monotone descent while ramping */
	CHK(buf[0] < 1.0f && buf[0] >= 0.5f);

	/* ---- (c) mute: 1.0 -> 0.0 ramps to silence within 64 samples and stays. */
	for (int i = 0; i < 64; i++) buf[i] = 1.0f;
	ret = reac_gain_ramp_block(buf, 64, 1.0f, 0.0f, STEP);
	CHK(ret == 0.0f);
	CHK(buf[63] == 0.0f);
	for (int i = 0; i < 64; i++)
		CHK(buf[i] >= 0.0f);        /* never goes negative on the way down */
	for (int i = 1; i < 64; i++)
		CHK(buf[i] <= buf[i - 1]);  /* monotone toward silence */
	/* Already-muted stays muted: a follow-on block at target 0 is all zeros. */
	for (int i = 0; i < 64; i++) buf[i] = 1.0f;
	ret = reac_gain_ramp_block(buf, 64, 0.0f, 0.0f, STEP);
	CHK(ret == 0.0f);
	for (int i = 0; i < 64; i++)
		CHK(buf[i] == 0.0f);

	/* ---- (d) per-channel independence: two channels stepped block-by-block the
	 * way the RT sink loop does (12-sample frames), each with its own cur. Ch0
	 * ramps toward 0.25; ch1 is held at unity. Ch1 must stay bit-exact 1.0 the
	 * whole time regardless of ch0's ramp, and ch0 must converge to 0.25. */
	float cur0 = 1.0f, cur1 = 1.0f;
	const float in_ch0 = 0.8f, in_ch1 = -0.6f;   /* distinct per-channel signals */
	float c0_last = 1.0f;
	for (int blk = 0; blk < 16; blk++) {
		float b0[REAC_SAMPLES_PER_PKT], b1[REAC_SAMPLES_PER_PKT];
		for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++) { b0[i] = in_ch0; b1[i] = in_ch1; }
		cur0 = reac_gain_ramp_block(b0, REAC_SAMPLES_PER_PKT, cur0, 0.25f, STEP);
		cur1 = reac_gain_ramp_block(b1, REAC_SAMPLES_PER_PKT, cur1, 1.0f, STEP);
		for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++) {
			/* ch1 untouched by ch0's ramp: exact input * unity */
			CHK(b1[i] == in_ch1);
			/* ch0 gain is monotone non-increasing across the whole run */
			float g0 = b0[i] / in_ch0;
			CHK(g0 <= c0_last + 1e-7f);
			c0_last = g0;
		}
	}
	CHK(cur1 == 1.0f);
	CHK(fabsf(cur0 - 0.25f) < 1e-6f);   /* ch0 landed on its target */

	/* ---- (e) boost: 1.0 -> 2.0 ramps UP and amplifies (over-amp headroom). */
	for (int i = 0; i < 64; i++) buf[i] = 1.0f;
	ret = reac_gain_ramp_block(buf, 64, 1.0f, 2.0f, STEP);
	CHK(fabsf(ret - 2.0f) < 1e-6f);
	CHK(buf[63] > 1.9f);                 /* boosted well above unity */
	for (int i = 1; i < 60; i++)
		CHK(buf[i] >= buf[i - 1]);       /* monotone climb */

	/* ---- (f) non-positive step snaps straight to target (no-ramp path). */
	for (int i = 0; i < 8; i++) buf[i] = 1.0f;
	ret = reac_gain_ramp_block(buf, 8, 1.0f, 0.3f, 0.0f);
	CHK(ret == 0.3f);
	for (int i = 0; i < 8; i++)
		CHK(buf[i] == 0.3f);            /* every sample already at target */

	printf("OK: gain kernel — unity no-op, target tracking, mute-to-zero, "
	       "per-channel independence, boost, snap\n");
	return 0;
}
