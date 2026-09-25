// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the UPSTREAM (stagebox->master) frame decode.
 *
 * Fixtures are REAL captured frames from the rig (reac-captures zoneA-48k /
 * zoneB-48k, MAC-sanitized to the stand-in 00:40:ab:c4:80:f6 per that repo's
 * convention): an S-1608 16-ch 628 B return, an S-0808 8-ch 340 B return, and
 * two consecutive S-4000 32-ch 1206 B OHRCA returns (matrix-m200-s4000
 * 2026-07-24; 1206 = 52 + 32*36 + the +2 CRC trailer after the end marker,
 * with a 1204 B trailerless variant on the same wire).
 * The expected planar PCM tables were produced by the capture-side analysis
 * that resolved the layout (2026-07-10, task #108): the upstream audio region
 * uses the obs-h8819 even/odd channel-pair byte BRAID (even ch = group bytes
 * 3,0,1; odd ch = 4,5,2 as LE lo/mid/hi), NOT the M-5000 downstream's plain
 * LE sample-major layout. Verified against the loud wired capture: the music
 * channel decodes at lag1 autocorrelation +0.998 under the braid vs noise
 * under plain LE.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <reac/reac.h>
#include <reac/reac_upstream.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#include "upstream_fixtures.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int32_t s24_at(const uint8_t *out, int nch, int ch, int s)
{
	(void)nch;
	const uint8_t *p = out + (size_t)(ch * REAC_SAMPLES_PER_PKT + s) * REAC_RESOLUTION;
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & REACPW_SAMPLE_SIGN)
		v |= (int32_t)~REACPW_SAMPLE_MASK;
	return v;
}

int main(void)
{
	/* 1. shape: nch from frame length (len = 52 + nch*36) */
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S1608_IN)) == REAC_BOX_S1608_IN);  /* S-1608 */
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S0808_IN)) == REAC_BOX_S0808_IN);   /* S-0808 */
	CHK(reac_upstream_channels(REAC_FRAME_BYTES) == -1); /* the desk's DOWNSTREAM shape is not upstream */
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S1608_IN) - 1) == -1);
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S1608_IN) + 1) == -1);
	CHK(reac_upstream_channels(REAC_FRAME_OVERHEAD) == -1);   /* nch 0 */
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(3)) == -1);  /* odd nch (3): braid needs channel pairs */
	CHK(reac_upstream_channels(0) == -1);
	CHK(reac_upstream_channels(REAC_HDR_COUNTER_OFF) == -1);

	/* 2. decode the captured 16-ch frame: header + counter + full PCM table */
	uint8_t out[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_upstream_decode(UP16, sizeof UP16, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP16) == 0xd9b1); /* same byte-14/15 LE counter as downstream */
	int bad = 0;
	for (int ch = 0; ch < 16; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			if (s24_at(out, 16, ch, s) != UP16_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 3. S-4000 32-ch OHRCA returns (1206 B = 52 + 32*36 + the +2 CRC trailer):
	 * ingest's door (reac_frame_clean_len(), <reac/reac.h>) strips the trailer
	 * BEFORE the parser ever sees the bytes — this parser now REFUSES a raw
	 * residue length instead of stripping it again (2026-09-21 ingest
	 * contract: the capture path's +2 is stripped AT THE DOOR, and every
	 * parser behind it refuses what ingest failed to strip). Both captured
	 * frames decode, through the door, to the independently-computed PCM
	 * tables, and the trailer is NOT decoded as audio — the 1206 B (cleaned)
	 * and 1204 B reads of the same frame are byte-equal. */
	CHK(reac_upstream_channels(sizeof UP32A) == -1); /* raw wire length — the residue is still on it */
	CHK(reac_frame_clean_len(sizeof UP32A) == REACPW_FRAME_LEN(REAC_BOX_S4000S_3208_IN)); /* the door's job, not this parser's */
	CHK(reac_upstream_channels(reac_frame_clean_len(1206)) == 32);
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S4000S_3208_IN)) == REAC_BOX_S4000S_3208_IN); /* trailerless variant, already clean */
	CHK(reac_upstream_channels(REACPW_FRAME_LEN(REAC_BOX_S4000S_3208_IN) + 1) == -1);
	ns = reac_upstream_decode(UP32A, reac_frame_clean_len(sizeof UP32A), out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP32A) == 0xff9c);
	bad = 0;
	for (int ch = 0; ch < 32; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			if (s24_at(out, 32, ch, s) != UP32A_PCM[ch][s])
				bad++;
	CHK(bad == 0);
	/* the trailer bytes never reach the audio: decoding the same real frame at
	 * its clean 1204 B length yields the identical planar PCM */
	uint8_t out2[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	CHK(reac_upstream_decode(UP32A, REACPW_FRAME_LEN(REAC_BOX_S4000S_3208_IN), out2) == REAC_SAMPLES_PER_PKT);
	CHK(memcmp(out, out2, (size_t)32 * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION) == 0);
	/* the second consecutive frame (counter +1) pins the per-frame stability,
	 * through the same door */
	ns = reac_upstream_decode(UP32B, reac_frame_clean_len(sizeof UP32B), out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP32B) == 0xff9d);
	bad = 0;
	for (int ch = 0; ch < 32; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			if (s24_at(out, 32, ch, s) != UP32B_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 4. decode the captured 8-ch frame */
	ns = reac_upstream_decode(UP8, sizeof UP8, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	bad = 0;
	for (int ch = 0; ch < 8; ch++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			if (s24_at(out, 8, ch, s) != UP8_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 5. validation rejects */
	uint8_t f[sizeof UP16];
	memcpy(f, UP16, sizeof f);
	f[REAC_ETHERTYPE_OFF] = 0x08; /* not the REAC EtherType */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	f[sizeof f - REAC_END_MARKER_BYTES] = 0x00; /* broken end marker */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	CHK(reac_upstream_decode(f, sizeof f - 1, out) == -1);          /* truncated */
	CHK(reac_upstream_decode(NULL, sizeof f, out) == -1);
	CHK(reac_upstream_decode(f, sizeof f, NULL) == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: upstream decode — 16-ch 628 B + 8-ch 340 B + 32-ch 1206/1204 B captured frames, braid "
	       "layout, full PCM match, shape/validation rejects\n");
	return 0;
}
