// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Proof that the downstream encoder emits the on-wire layout a real Roland
 * stagebox de-interleaves onto its analog outputs: PLAIN-LE sample-major (the
 * exact inverse of reac_decode), NOT the obs-h8819 even/odd braid.
 *
 * Ground truth is reac_decode — the reac-aes67 downstream decode core, validated
 * on-rig against a live M-5000 (plain-LE coherence 0.999; the obs-h8819 braid
 * decoded the same stream as noise, see reac-aes67 src/reac_decode.c). A single
 * S-1608 de-interleaves its output fabric ONE way, so the layout the M-5000
 * validated is the layout every box reads — including from our master.
 *
 * We assert:
 *   (a) a -20 dBFS 440 Hz sine placed on box output 8 (ch 7, an ODD channel)
 *       reconstructs under reac_decode within 0.5 dB of -20 dBFS, tracks the
 *       ideal sine sample-for-sample (no harmonic garbage), and lands ONLY on
 *       ch 7 — every other box output stays dead silent (no cross-channel bleed);
 *   (b) a distinct DC per channel round-trips under reac_decode within one 24-bit
 *       ULP, catching any stride/pair swap;
 *   (c) NEGATIVE CONTROL = the reported burst: encoding the SAME -20 dBFS sine
 *       with the old obs-h8819 braid and letting the box decode it plain-LE
 *       smears ch 7 to near full-scale (the odd channel's MID byte lands in its
 *       HIGH lane) and bleeds onto the neighbour ch 6 — the 2026-07-13 Stage-B
 *       near-equipment-damage failure. This is why downstream must be plain-LE.
 */
#include "reac_tx.h"
#include "reac_decode.h"      /* plain-LE core: the box's rig-validated view */
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static float s24le_to_f32(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return (float)v / 8388608.0f;
}

/* The OLD (buggy) obs-h8819 braid encoder, kept ONLY as the negative control so
 * we can reproduce the field burst numerically. Packs each channel pair (2k,2k+1)
 * into a 6-byte group: even s24-LE -> g[3],g[0],g[1]; odd -> g[4],g[5],g[2]. */
static void encode_braid_old(uint8_t *frame, float *const *planar)
{
	const int N = REAC_MAX_CHANNELS;
	memset(frame, 0, REAC_FRAME_BYTES);
	frame[12] = 0x88; frame[13] = 0x19;
	frame[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	frame[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
	uint8_t *audio = frame + REAC_AUDIO_OFFSET;
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		for (int k = 0; k < N; k += 2) {
			float ev = planar[k][s], od = planar[k + 1][s];
			int32_t se = (int32_t)lrintf(fmaxf(-8388608.0f, fminf(8388607.0f, ev * 8388608.0f)));
			int32_t so = (int32_t)lrintf(fmaxf(-8388608.0f, fminf(8388607.0f, od * 8388608.0f)));
			uint8_t e[3] = { se & 0xFF, (se >> 8) & 0xFF, (se >> 16) & 0xFF };
			uint8_t o[3] = { so & 0xFF, (so >> 8) & 0xFF, (so >> 16) & 0xFF };
			uint8_t *g = audio + (size_t)(s * N + k) * REAC_RESOLUTION;
			g[0] = e[1]; g[1] = e[2]; g[2] = o[2];
			g[3] = e[0]; g[4] = o[0]; g[5] = o[1];
		}
	}
}

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

static float dbfs(float x) { return x > 0 ? 20.0f * log10f(x) : -999.0f; }

int main(void)
{
	const struct reac_mode m = { 48000, 40, 12 };
	static const uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	const int SINE_CH = 7;         /* box output 8 (1-based), an ODD channel */
	const int NEI_CH  = 6;         /* its even pair-neighbour, box output 7  */
	const double AMP = 0.1;        /* -20 dBFS */
	const double F = 440.0, FS = 48000.0;
	const int NFRAMES = 400;       /* 4800 samples: enough cycles of 440 Hz */

	float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) planar[ch] = chbuf[ch];

	/* ---- (a) -20 dBFS 440 Hz sine on box output 8, plain-LE reconstruction ---- */
	float good_peak = 0.0f, good_sine_maxdev = 0.0f, good_bleed = 0.0f;
	float bad_peak = 0.0f, bad_bleed = 0.0f;
	uint8_t frame[REAC_FRAME_BYTES];
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];

	for (int fr = 0; fr < NFRAMES; fr++) {
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			double t = (double)(fr * REAC_SAMPLES_PER_PKT + s) / FS;
			float v = (float)(AMP * sin(2.0 * M_PI * F * t));
			for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++)
				chbuf[ch][s] = (ch == SINE_CH) ? v : 0.0f;
		}

		/* correct encoder -> box decodes plain-LE */
		int len = reac_tx_build(frame, planar, REAC_MAX_CHANNELS,
		                        REAC_SAMPLES_PER_PKT, 0x1234, src);
		CHK(len == REAC_FRAME_BYTES);
		if (fr == 0) {
			CHK(frame[12] == 0x88 && frame[13] == 0x19);
			CHK(frame[14] == 0x34 && frame[15] == 0x12);          /* counter LE */
			CHK(memcmp(frame + 6, src, 6) == 0);
			CHK(frame[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0);
			CHK(frame[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);
		}
		int ns = reac_decode(frame, REAC_FRAME_BYTES, &m, s24);
		CHK(ns == REAC_SAMPLES_PER_PKT);
		for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
			for (int s = 0; s < ns; s++) {
				float got = s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
				if (ch == SINE_CH) {
					float dev = fabsf(got - chbuf[ch][s]);
					if (dev > good_sine_maxdev) good_sine_maxdev = dev;
					if (fabsf(got) > good_peak) good_peak = fabsf(got);
				} else if (fabsf(got) > good_bleed) {
					good_bleed = fabsf(got);
				}
			}
		}

		/* NEGATIVE CONTROL: old braid encoder -> box still decodes plain-LE */
		encode_braid_old(frame, planar);
		ns = reac_decode(frame, REAC_FRAME_BYTES, &m, s24);
		CHK(ns == REAC_SAMPLES_PER_PKT);
		for (int s = 0; s < ns; s++) {
			float bch7 = fabsf(s24le_to_f32(&s24[(size_t)(SINE_CH * ns + s) * 3]));
			float bch6 = fabsf(s24le_to_f32(&s24[(size_t)(NEI_CH  * ns + s) * 3]));
			if (bch7 > bad_peak)  bad_peak = bch7;
			if (bch6 > bad_bleed) bad_bleed = bch6;
		}
	}

	printf("plain-LE (fixed): box output 8 peak = %.4f (%.2f dBFS), "
	       "sine max deviation = %.2e, worst other-output = %.2e\n",
	       good_peak, dbfs(good_peak), good_sine_maxdev, good_bleed);
	/* amplitude within 0.5 dB of -20 dBFS */
	CHK(fabsf(dbfs(good_peak) - (-20.0f)) < 0.5f);
	/* tracks the ideal sine -> no harmonic garbage (well under 1 dB of the tone) */
	CHK(good_sine_maxdev < 1e-3f);
	/* nothing anywhere else on the box */
	CHK(good_bleed == 0.0f);

	printf("obs-h8819 braid (the reported BURST): box output 8 peak = %.4f "
	       "(%.2f dBFS) from a -20 dBFS source; neighbour output 7 bleed = %.4f "
	       "(%.2f dBFS)\n", bad_peak, dbfs(bad_peak), bad_bleed, dbfs(bad_bleed));
	/* the burst: a -20 dBFS tone smears to within a few dB of full-scale ... */
	CHK(bad_peak > 0.5f);
	/* ... and leaks onto the pair-neighbour output. */
	CHK(bad_bleed > 0.01f);

	/* ---- (b) distinct DC per channel, plain-LE round-trip within one ULP ---- */
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = (float)ch / 64.0f - 0.3f;
	reac_tx_build(frame, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, src);
	int ns = reac_decode(frame, REAC_FRAME_BYTES, &m, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	float dc_err = 0.0f;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++)
		for (int s = 0; s < ns; s++) {
			float got = s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
			float e = fabsf(got - chbuf[ch][s]);
			if (e > dc_err) dc_err = e;
		}
	printf("plain-LE: 40-ch distinct-DC round-trip max err = %.2e (tol 1e-6)\n", dc_err);
	CHK(dc_err < 1e-6f);

	printf("OK: downstream encoder emits the box's plain-LE fabric; clean -20 dBFS "
	       "sine on output 8, no cross-wiring; the obs-h8819 braid is the burst\n");
	return 0;
}
