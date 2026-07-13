// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for the SPSC ring: capacity rounding, planar round-trip, underrun
 * zero-fill, and overrun drop-oldest. No PipeWire / libreac needed. */

#include "../src/reac_ring.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

#define CH 4

int main(void)
{
	struct reac_ring r;
	assert(reac_ring_init(&r, CH, 100) == 0);
	assert(r.capacity == 128);          /* rounded up to power of two */
	assert(reac_ring_readable(&r) == 0);

	/* init guard: a zero or overflow-rounding capacity must FAIL (-1), not build a
	 * 0-slot ring with mask 0xFFFFFFFF. A garbage --rate reaches reac_ring_init as
	 * such a depth (sample_rate/4 of a negative rate), and the first write would
	 * otherwise scribble the heap. */
	struct reac_ring bad;
	assert(reac_ring_init(&bad, CH, 0) == -1);            /* zero capacity */
	assert(reac_ring_init(&bad, CH, 0xFFFFFFFFu) == -1);  /* next_pow2 wraps to 0 */
	assert(reac_ring_init(&bad, CH, 0x80000001u) == -1);  /* rounds past UINT32_MAX */

	/* write 12 frames/ch (a REAC quantum), planar src[c*n + s] = c*100 + s */
	float src[CH * 12];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 12; s++)
			src[c * 12 + s] = (float)(c * 100 + s);
	assert(reac_ring_write(&r, src, 12) == 12);
	assert(reac_ring_readable(&r) == 12);

	/* read 8 frames/ch back into planar dst, check values. dst buffers are sized
	 * 16 so the later 10-frame read can't overflow them. */
	float d0[16], d1[16], d2[16], d3[16];
	float *dst[CH] = { d0, d1, d2, d3 };
	uint32_t got = reac_ring_read_planar(&r, dst, CH, 8);
	assert(got == 8);
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 8; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + s)) < 1e-6f);
	assert(reac_ring_readable(&r) == 4);

	/* underrun: ask for 10 with only 4 left -> 6 zeros, underrun bumped */
	got = reac_ring_read_planar(&r, dst, CH, 10);
	assert(got == 4);
	for (int c = 0; c < CH; c++) {
		for (int s = 0; s < 4; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + 8 + s)) < 1e-6f);
		for (int s = 4; s < 10; s++)
			assert(dst[c][s] == 0.0f);
	}
	assert(atomic_load(&r.underruns) == 6);

	/* over-capacity write: request 200 into an empty 128-slot ring. The write
	 * count is bounded to writable (mask == 127) and the remainder is dropped as
	 * overrun, but the SOURCE stride stays 200 — so channel c is read from
	 * big[c*200 + s], NOT from a clamped stride. Distinct per-channel marker
	 * (c*1000 + s) catches the old clamp bug, which would have made channels c>0
	 * read each other's samples. */
	float big[CH * 200];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 200; s++)
			big[c * 200 + s] = (float)(c * 1000 + s);
	uint64_t ovbig = atomic_load(&r.overruns);
	uint32_t w = reac_ring_write(&r, big, 200);
	assert(w == r.mask);                /* wrote one ring's worth (127) */
	assert(reac_ring_readable(&r) == r.mask);
	assert(atomic_load(&r.overruns) == ovbig + (200 - r.mask)); /* 73 dropped */
	/* read it all back and verify NO cross-channel corruption: channel c must hold
	 * big[c*200 + s] for s in [0,127). */
	float v0[128], v1[128], v2[128], v3[128];
	float *vdst[CH] = { v0, v1, v2, v3 };
	uint32_t vg = reac_ring_read_planar(&r, vdst, CH, r.mask);
	assert(vg == r.mask);
	for (int c = 0; c < CH; c++)
		for (uint32_t s = 0; s < r.mask; s++)
			assert(fabsf(vdst[c][s] - (float)(c * 1000 + (int)s)) < 1e-6f);

	/* refill to full to exercise the transient drop-newest overrun path below */
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 200; s++)
			big[c * 200 + s] = (float)s;
	w = reac_ring_write(&r, big, 200);
	assert(w == r.mask);                /* fills the empty ring back to full */
	assert(reac_ring_readable(&r) == r.mask);

	/* real overrun: ring is now full (readable == mask, writable == 0). A further
	 * write drops the NEWEST (SPSC-safe: the producer must not move tail), so it
	 * writes 0, readable is unchanged, and overruns is bumped by the dropped count. */
	uint64_t ov0 = atomic_load(&r.overruns);
	float more[CH * 4];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 4; s++)
			more[c * 4 + s] = -1.0f;
	uint32_t w2 = reac_ring_write(&r, more, 4);
	assert(w2 == 0);
	assert(reac_ring_readable(&r) == r.mask);
	assert(atomic_load(&r.overruns) == ov0 + 4);

	reac_ring_free(&r);
	printf("test_reac_ring: OK\n");
	return 0;
}
