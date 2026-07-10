// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the UPSTREAM (stagebox->master) frame decode.
 *
 * Fixtures are REAL captured frames from the rig (reac-captures zoneA-48k /
 * zoneB-48k, MAC-sanitized to the stand-in 00:40:ab:c4:80:f6 per that repo's
 * convention): an S-1608 16-ch 628 B return and an S-0808 8-ch 340 B return.
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
#include "reac_upstream.h"

#include "upstream_fixtures.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int32_t s24_at(const uint8_t *out, int nch, int ch, int s)
{
	(void)nch;
	const uint8_t *p = out + (size_t)(ch * REAC_SAMPLES_PER_PKT + s) * 3;
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
	if (v & 0x00800000)
		v |= ~0x00FFFFFF;
	return v;
}

int main(void)
{
	/* 1. shape: nch from frame length (len = 52 + nch*36) */
	CHK(reac_upstream_channels(628) == 16);  /* S-1608 */
	CHK(reac_upstream_channels(340) == 8);   /* S-0808 */
	CHK(reac_upstream_channels(1492) == -1); /* the 40-ch DOWNSTREAM shape is not upstream */
	CHK(reac_upstream_channels(627) == -1);
	CHK(reac_upstream_channels(629) == -1);
	CHK(reac_upstream_channels(52) == -1);   /* nch 0 */
	CHK(reac_upstream_channels(160) == -1);  /* odd nch (3): braid needs channel pairs */
	CHK(reac_upstream_channels(0) == -1);
	CHK(reac_upstream_channels(14) == -1);

	/* 2. decode the captured 16-ch frame: header + counter + full PCM table */
	uint8_t out[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_upstream_decode(UP16, sizeof UP16, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(reac_frame_counter(UP16) == 0xd9b1); /* same byte-14/15 LE counter as downstream */
	int bad = 0;
	for (int ch = 0; ch < 16; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 16, ch, s) != UP16_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 3. decode the captured 8-ch frame */
	ns = reac_upstream_decode(UP8, sizeof UP8, out);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	bad = 0;
	for (int ch = 0; ch < 8; ch++)
		for (int s = 0; s < 12; s++)
			if (s24_at(out, 8, ch, s) != UP8_PCM[ch][s])
				bad++;
	CHK(bad == 0);

	/* 4. validation rejects */
	uint8_t f[628];
	memcpy(f, UP16, sizeof f);
	f[12] = 0x08; /* not 0x8819 */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	f[626] = 0x00; /* broken end marker */
	CHK(reac_upstream_decode(f, sizeof f, out) == -1);
	memcpy(f, UP16, sizeof f);
	CHK(reac_upstream_decode(f, 627, out) == -1);          /* truncated */
	CHK(reac_upstream_decode(NULL, 628, out) == -1);
	CHK(reac_upstream_decode(f, sizeof f, NULL) == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: upstream decode — 16-ch 628 B + 8-ch 340 B captured frames, braid "
	       "layout, full PCM match, shape/validation rejects\n");
	return 0;
}
