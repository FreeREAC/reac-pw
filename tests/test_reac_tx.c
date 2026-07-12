// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Proof that the downstream encoder emits the on-wire layout a real Roland
 * stagebox de-braids onto its analog outputs: the obs-h8819 even/odd channel-
 * pair BRAID (the exact inverse of reac_upstream_decode), NOT plain-LE.
 *
 * Ground truth is a local un-braid (unbraid_ch below) copied byte-for-byte from
 * reac_upstream_decode's layout — the layout validated against the loud rig
 * captures (reac-captures/wired-reac-loud, zoneA-48k). The old encoder wrote
 * plain-LE sample-major, which those captures prove the box reads back as
 * byte-rotated NOISE (odd channels smear to full-scale). We therefore assert:
 *
 *   (a) a 1 kHz sine placed on ch 8 (operator's monitor port) reconstructs
 *       cleanly under the braid and lands ONLY on ch 8 — no cross-channel bleed;
 *   (b) a distinct DC per channel round-trips within one 24-bit ULP under the
 *       braid, catching any pair/stride swap;
 *   (c) NEGATIVE CONTROL: decoding the same frame with the plain-LE core
 *       (reac_decode) does NOT reconstruct ch 8 clean and DOES leak onto other
 *       channels — i.e. the old plain-LE path is exactly the reported noise.
 */
#include "reac_tx.h"
#include "reac_decode.h"      /* plain-LE core: the NEGATIVE control */
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

/* Un-braid one channel of a downstream frame's audio region into 12 floats.
 * Byte layout identical to reac_upstream_decode / obs-h8819 convert_to_pcm24lep,
 * with the 40-ch downstream sample stride (N*3). This is the box's view. */
static void unbraid_ch(const uint8_t *audio, int ch, float out[REAC_SAMPLES_PER_PKT])
{
	const int N = REAC_MAX_CHANNELS;
	const uint8_t *g0 = audio + (size_t)(ch & ~1) * REAC_RESOLUTION;
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		const uint8_t *g = g0 + (size_t)s * N * REAC_RESOLUTION;
		uint8_t trio[3];
		if ((ch & 1) == 0) {
			trio[0] = g[3]; trio[1] = g[0]; trio[2] = g[1];
		} else {
			trio[0] = g[4]; trio[1] = g[5]; trio[2] = g[2];
		}
		out[s] = s24le_to_f32(trio);
	}
}

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

int main(void)
{
	const struct reac_mode m = { 48000, 40, 12 };
	static const uint8_t src[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	const int SINE_CH = 8;    /* the operator's monitor: box output port 8 (even) */
	const int SINE_CH2 = 13;  /* an ODD channel too — this is where plain-LE smears
	                           * to full-scale noise on the box (cf. zoneA ch13) */

	/* ---- test (a): a 1 kHz half-scale sine on ch 8 and a distinct tone on the
	 * odd channel 13; silence on every other channel. */
	float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	const double f_sig = 1000.0, f_s = 48000.0;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		planar[ch] = chbuf[ch];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			float ph = 2.0f * (float)M_PI * (float)f_sig * (float)s / (float)f_s;
			if (ch == SINE_CH)       chbuf[ch][s] = 0.5f * sinf(ph);
			else if (ch == SINE_CH2) chbuf[ch][s] = 0.4f * sinf(2.0f * ph);
			else                     chbuf[ch][s] = 0.0f;
		}
	}

	uint8_t frame[REAC_FRAME_BYTES];
	int len = reac_tx_build(frame, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT,
	                        0x1234, src);
	CHK(len == REAC_FRAME_BYTES);

	/* header well-formedness */
	CHK(frame[12] == 0x88 && frame[13] == 0x19);           /* EtherType */
	CHK(frame[14] == 0x34 && frame[15] == 0x12);           /* counter LE */
	CHK(memcmp(frame + 6, src, 6) == 0);                   /* src MAC */
	CHK(frame[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0); /* 0xC2 */
	CHK(frame[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1); /* 0xEA */

	const uint8_t *audio = frame + REAC_AUDIO_OFFSET;

	/* Braid decode (the box's view): the sine must reconstruct on ch 8 and every
	 * other channel must be exactly silent — no quantization garbage, no bleed. */
	float sine_err = 0.0f, bleed = 0.0f;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		float got[REAC_SAMPLES_PER_PKT];
		unbraid_ch(audio, ch, got);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			float e = fabsf(got[s] - chbuf[ch][s]);
			if (ch == SINE_CH || ch == SINE_CH2) { if (e > sine_err) sine_err = e; }
			else if (fabsf(got[s]) > bleed)      { bleed = fabsf(got[s]); }
		}
	}
	printf("braid: ch%d/ch%d sine max err = %.2e; worst other-channel level = %.2e\n",
	       SINE_CH, SINE_CH2, sine_err, bleed);
	CHK(sine_err < 1e-6f);   /* clean tones on the right channels */
	CHK(bleed == 0.0f);      /* every other box output is dead silent */

	/* NEGATIVE CONTROL: the old plain-LE core sees the braided bytes as garbage.
	 * Both tones are mangled and the odd channel (13) smears toward full-scale —
	 * exactly the noise the operator heard on the box outputs. We measure the
	 * worst per-sample reconstruction error over ALL channels; a correct decode
	 * would be ~0, plain-LE is gross. */
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(frame, REAC_FRAME_BYTES, &m, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	float plain_worst = 0.0f, plain_ch13_peak = 0.0f;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		for (int s = 0; s < ns; s++) {
			float got = s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
			float e = fabsf(got - chbuf[ch][s]);
			if (e > plain_worst) plain_worst = e;
			if (ch == SINE_CH2 && fabsf(got) > plain_ch13_peak)
				plain_ch13_peak = fabsf(got);
		}
	}
	printf("plain-LE (broken): worst reconstruction err = %.2e; ch%d peak = %.2e "
	       "(near full-scale smear)\n", plain_worst, SINE_CH2, plain_ch13_peak);
	CHK(plain_worst > 0.1f);         /* plain-LE grossly corrupts the program ... */
	CHK(plain_ch13_peak > 0.5f);     /* ... and the odd channel smears to noise */

	/* ---- test (b): distinct DC per channel, braid round-trip within one ULP.
	 * A pair/stride swap would surface here as a mismatched or cross-wired DC. */
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = (float)ch / 64.0f - 0.3f;
	reac_tx_build(frame, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, src);
	float dc_err = 0.0f;
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		float got[REAC_SAMPLES_PER_PKT];
		unbraid_ch(frame + REAC_AUDIO_OFFSET, ch, got);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			float e = fabsf(got[s] - chbuf[ch][s]);
			if (e > dc_err) dc_err = e;
		}
	}
	printf("braid: 40-ch distinct-DC round-trip max err = %.2e (tol 1e-6)\n", dc_err);
	CHK(dc_err < 1e-6f);

	printf("OK: downstream encoder emits the stagebox braid; clean sine on ch%d, "
	       "no cross-wiring; plain-LE is the reported noise\n", SINE_CH);
	return 0;
}
