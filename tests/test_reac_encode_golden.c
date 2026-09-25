// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* BYTE-IDENTITY gate for the encoder's move into libreac (2026-07-29).
 *
 * reac_tx_build and reac_ctrl.c's static place_braided_audio were the same
 * braid loop written twice; both moved to libreac as reac_downstream_build()
 * and reac_braid_encode() (<reac/reac_encode.h>). reac-pw's main auto-deploys
 * to the live rig, so the move had exactly one acceptance criterion: the bytes
 * leaving the NIC must not change. Not "the tests still pass" — the BYTES.
 *
 * This file is that gate. It generates a large deterministic corpus of frames
 * and digests every one of them (length included, so a size change is caught
 * too) with FNV-1a-64. The two expected constants were produced by compiling
 * this same corpus against the PRE-MOVE tree at c86a2d6 — i.e. they encode what
 * the rig was already receiving. A mismatch means the wire format changed; it
 * is never a reason to update the constant.
 *
 *   DOWNSTREAM  1120 master broadcast frames: {0,1,2,7,8,16,32,40} source
 *               channels x {0,1,6,12,13} samples x 7 counters (0, 1, 0x1234,
 *               0x7fff, 0x8000, 0xfffe, 0xffff — both sides of the 16-bit
 *               wrap) x 4 input scales (quiet, full scale, 4x CLIPPING,
 *               near-denormal), every 5th plane NULL. libreac's
 *               tests/test_encode.c pins this SAME constant from the other side
 *               of the wrap, so the two repos cannot drift apart silently.
 *
 *   UPSTREAM    9240 box->master frames over every real box width (2, 8, 16,
 *               32, 38) plus the 40-ch shape that must be REJECTED, across all
 *               eleven reac_ctrl_build_* entry points. Six of them carry audio
 *               (upstream FILLER, presence-flood, and the four cold-connect
 *               variants) — those are what exercise the moved braid loop. The
 *               five audio-free ones (heartbeat, config-announce, name frame,
 *               extra frame, head-amp) are digested too, which is how this test
 *               also proves the refactor left the HANDSHAKE path — control
 *               block, both nested checksums, the box-model matrix — untouched.
 *               The NULL-planar path is exercised at every width.
 */
#include <reac/reac_ctrl.h>

#include <reac/reac.h>
#include <reac/reac_encode.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

/* Digests of the PRE-MOVE encoder's output over the corpora below (reac-pw at
 * c86a2d6, libreac at 498f411). Reproducing them needs that old code, so a
 * mismatch is a wire-format regression, not a stale constant. */
#define DOWNSTREAM_GOLDEN 0xe86e36e1d979f244ULL
#define UPSTREAM_GOLDEN   0xd55c2aca88584655ULL

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- deterministic corpus ---- */

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint32_t rng_state;
static void rng_seed(uint32_t s) { rng_state = s ? s : 0x2b3c4d5eu; }
static uint32_t rng_u32(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }
static float rng_f(float scale)
{
	int32_t v = (int32_t)rng_u32();
	return scale * ((float)v / 2147483648.0f);
}

static uint64_t fnv;
static void absorb(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		fnv = (fnv ^ p[i]) * FNV_PRIME;
}
static void absorb_len(size_t len)
{
	uint8_t l[2] = { (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff) };
	absorb(l, 2);
}

static const float SCALES[4] = { 0.25f, 1.0f, 4.0f, 0.0000001f };
static const int   NS[5]     = { 0, 1, 6, REAC_SAMPLES_PER_PKT, REAC_SAMPLES_PER_PKT + 1 };
static const uint16_t CNT[7] = { 0x0000, 0x0001, 0x1234, 0x7fff, 0x8000, 0xfffe, 0xffff };

static float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
static float *planar[REAC_MAX_CHANNELS];
static uint8_t frame[2048];

static float *const *fill(uint32_t seed, float scale, int null_all)
{
	rng_seed(seed);
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = rng_f(scale);
		planar[ch] = (ch % 5 == 4) ? NULL : chbuf[ch];
	}
	return null_all ? NULL : (float *const *)planar;
}

static uint64_t downstream_corpus(void)
{
	static const int NCH[8] = { 0, 1, 2, 7, 8, 16, 32, REAC_MAX_CHANNELS };
	uint32_t seed = 1;

	fnv = FNV_INIT;
	for (int si = 0; si < 4; si++)
		for (int ci = 0; ci < 8; ci++)
			for (int ni = 0; ni < 5; ni++)
				for (int ti = 0; ti < 7; ti++) {
					int nch = NCH[ci];
					/* planar==NULL only with nch==0: the pre-move encoder
					 * dereferenced planar[ch] for ch<nch, so a NULL planar with
					 * channels was never a legal call and is not pinned here. */
					float *const *pl = fill(seed++, SCALES[si], nch == 0);
					uint8_t src[6] = { 0x00, 0x40, 0xab,
					                   (uint8_t)si, (uint8_t)ci, (uint8_t)(ni * 8 + ti) };
					int len = reac_downstream_build(frame, pl, nch, NS[ni], CNT[ti], src);
					absorb_len((size_t)len);
					absorb(frame, (size_t)len);
				}
	return fnv;
}

static uint64_t upstream_corpus(void)
{
	static const int NCH[6] = { 2, 8, 16, 32, REAC_MAX_CHANNELS - REAC_BRAID_PAIR_CHANNELS,
	                               REAC_MAX_CHANNELS };
	static const uint8_t master[6] = { 0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e };
	static const uint8_t bcast[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint32_t seed = 0x50000;

	fnv = FNV_INIT;
	for (int si = 0; si < 4; si++)
		for (int ci = 0; ci < 6; ci++)
			for (int ni = 0; ni < 5; ni++)
				for (int ti = 0; ti < 7; ti++) {
					int nch = NCH[ci];
					int ns = NS[ni];
					uint16_t cnt = CNT[ti];
					/* the builders guard the planar pointer, so the NULL path is
					 * exercised here at every width */
					int null_all = ((si + ci + ni + ti) % 7) == 0;
					float *const *pl = fill(seed++, SCALES[si], null_all);
					uint8_t src[6] = { 0x00, 0x40, 0xab,
					                   (uint8_t)si, (uint8_t)ci, (uint8_t)(ni * 8 + ti) };
					size_t len;

					/* the six audio-carrying box->master builders: the moved braid loop */
					len = reac_ctrl_build_upstream_filler(frame, master, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_flood_filler(frame, bcast, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_coldconnect(frame, master, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_coldconnect_0013(frame, master, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_coldconnect_0016(frame, master, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_coldconnect_001a(frame, master, src, cnt, nch, pl, ns);
					absorb_len(len); absorb(frame, len);

					/* the audio-free handshake frames: pinned so the refactor is
					 * shown to have left the control plane alone */
					len = reac_ctrl_build_box_hb(frame, master, src, cnt, nch);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_config_announce(frame, master, src, cnt, nch);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_identity_first(frame, master, src, cnt, nch);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_identity_last(frame, master, src, cnt, nch);
					absorb_len(len); absorb(frame, len);
					len = reac_ctrl_build_headamp(frame, bcast, src, cnt,
					                              (uint8_t)(nch & 0x3f),
					                              (uint8_t)(ti % 3), (uint8_t)(si));
					absorb_len(len); absorb(frame, len);
				}
	return fnv;
}

int main(void)
{
	uint64_t d = downstream_corpus();
	uint64_t u = upstream_corpus();

	if (d != DOWNSTREAM_GOLDEN) {
		fails++;
		fprintf(stderr,
		        "FAIL downstream corpus digest: got 0x%016llx want 0x%016llx\n"
		        "  the BYTES of the master broadcast changed — wire-format regression\n",
		        (unsigned long long)d, (unsigned long long)DOWNSTREAM_GOLDEN);
	}
	if (u != UPSTREAM_GOLDEN) {
		fails++;
		fprintf(stderr,
		        "FAIL upstream corpus digest: got 0x%016llx want 0x%016llx\n"
		        "  the BYTES of a box->master frame changed — wire-format regression\n",
		        (unsigned long long)u, (unsigned long long)UPSTREAM_GOLDEN);
	}

	/* Sanity: the corpus must actually have built frames, so a builder that
	 * started returning 0 everywhere cannot pass by digesting nothing. */
	CHK(reac_ctrl_build_upstream_filler(frame, (const uint8_t[6]){0}, (const uint8_t[6]){0},
	                                    0, 16, NULL, REAC_SAMPLES_PER_PKT) == REACPW_FRAME_LEN(16));
	CHK(reac_downstream_build(frame, NULL, 0, REAC_SAMPLES_PER_PKT, 0,
	                          (const uint8_t[6]){0}) == REAC_FRAME_BYTES);

	if (fails) {
		fprintf(stderr, "test_reac_encode_golden: %d failure(s)\n", fails);
		return 1;
	}
	printf("OK: encode byte-identity — downstream 0x%016llx, upstream 0x%016llx "
	       "(both match the pre-libreac-move encoder)\n",
	       (unsigned long long)d, (unsigned long long)u);
	return 0;
}
