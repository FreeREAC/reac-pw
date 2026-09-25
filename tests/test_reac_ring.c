// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for the SPSC ring: capacity rounding, planar round-trip, underrun
 * zero-fill, overrun drop-oldest, and the NARROW-SOURCE contract. No PipeWire /
 * libreac needed. */

#include <reac/transport/reac_ring.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

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
	float src[CH * REAC_SAMPLES_PER_PKT];
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			src[c * REAC_SAMPLES_PER_PKT + s] = (float)(c * 100 + s);
	assert(reac_ring_write(&r, src, REAC_SAMPLES_PER_PKT, CH) == REAC_SAMPLES_PER_PKT);
	assert(reac_ring_readable(&r) == REAC_SAMPLES_PER_PKT);

	/* read 8 frames/ch back into planar dst, check values. dst buffers are sized
	 * 16 so the later 10-frame read can't overflow them. */
	const uint32_t LEFT = REAC_SAMPLES_PER_PKT - 8;   /* what the 8-frame read leaves */
	float d0[16], d1[16], d2[16], d3[16];
	float *dst[CH] = { d0, d1, d2, d3 };
	uint32_t got = reac_ring_read_planar(&r, dst, CH, 8);
	assert(got == 8);
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < 8; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + s)) < 1e-6f);
	assert(reac_ring_readable(&r) == LEFT);

	/* underrun: ask for 10 with only LEFT (4) left -> the rest zeros, underrun bumped */
	got = reac_ring_read_planar(&r, dst, CH, 10);
	assert(got == LEFT);
	for (int c = 0; c < CH; c++) {
		for (int s = 0; s < (int)LEFT; s++)
			assert(fabsf(dst[c][s] - (float)(c * 100 + 8 + s)) < 1e-6f);
		for (int s = (int)LEFT; s < 10; s++)
			assert(dst[c][s] == 0.0f);
	}
	assert(atomic_load(&r.underruns) == 10 - LEFT);

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
	uint32_t w = reac_ring_write(&r, big, 200, CH);
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
	w = reac_ring_write(&r, big, 200, CH);
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
	uint32_t w2 = reac_ring_write(&r, more, 4, CH);
	assert(w2 == 0);
	assert(reac_ring_readable(&r) == r.mask);
	assert(atomic_load(&r.overruns) == ov0 + 4);

	reac_ring_free(&r);

	/* ---- the narrow-source contract ------------------------------------- *
	 * The producer writes only `src_channels` rows so an 8-channel box stops
	 * paying for 32 rows of silence. Two separate mechanisms keep the rows above
	 * that width silent, and each has its own case below because each has its own
	 * failure. Both are asserted on the DATA a consumer reads, never on a return
	 * count -- a short write and a stale row look identical from the count.
	 *
	 * The first version of this test asserted both and caught NEITHER sabotage,
	 * because it read back only the slots just written: a stale row's stale data
	 * sits at the ring positions the head has already moved past, so it stays
	 * invisible until the head WRAPS onto it. Any test of this contract has to
	 * cycle the ring further than its capacity. That is what CYCLES is for. */
#define CYCLES 8                    /* 8 x 12 = 96 frames through a 64-slot ring */
	struct reac_ring nr;
	assert(reac_ring_init(&nr, CH, 64) == 0);
	float nbuf[CH * REAC_SAMPLES_PER_PKT];
	float n0[REAC_SAMPLES_PER_PKT], n1[REAC_SAMPLES_PER_PKT], n2[REAC_SAMPLES_PER_PKT], n3[REAC_SAMPLES_PER_PKT];
	float *ndst[CH] = { n0, n1, n2, n3 };

	/* 1. AN UNTOUCHED ROW READS AS SILENCE -- the allocator's guarantee.
	 *    A fresh ring written narrow from the very first frame retires nothing
	 *    (wrote_channels starts at 0), so rows 2..3 are silent only because
	 *    reac_ring_init used calloc. Sabotage: swap that calloc for a malloc and
	 *    this case is the one that fails. */
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			nbuf[c * REAC_SAMPLES_PER_PKT + s] = (float)(c + 1) * 10.0f;
	for (int k = 0; k < CYCLES; k++) {
		assert(reac_ring_write(&nr, nbuf, REAC_SAMPLES_PER_PKT, 2) == REAC_SAMPLES_PER_PKT);
		assert(reac_ring_read_planar(&nr, ndst, CH, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			assert(fabsf(n0[s] - 10.0f) < 1e-6f);
			assert(fabsf(n1[s] - 20.0f) < 1e-6f);
			assert(n2[s] == 0.0f);          /* never written -- allocator silence */
			assert(n3[s] == 0.0f);
		}
	}

	/* 2. A SHRINKING SOURCE RETIRES THE ROWS IT ABANDONS -- the write's guarantee.
	 *    Fill EVERY slot of rows 2..3 by cycling a wide source past the ring's
	 *    capacity, then narrow. Without the retire, rows 2..3 keep replaying the
	 *    wide source as the head wraps back onto the slots it left behind: a box
	 *    swap would leave the previous box's microphones live on channels the new
	 *    box does not have, fading in as the ring came round. Sabotage: delete the
	 *    memset loop in reac_ring_write and this case fails. */
	for (int c = 0; c < CH; c++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			nbuf[c * REAC_SAMPLES_PER_PKT + s] = 77.0f;
	for (int k = 0; k < CYCLES; k++) {
		assert(reac_ring_write(&nr, nbuf, REAC_SAMPLES_PER_PKT, CH) == REAC_SAMPLES_PER_PKT);
		assert(reac_ring_read_planar(&nr, ndst, CH, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
	}
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
		assert(fabsf(n3[s] - 77.0f) < 1e-6f);   /* the wide source really landed */

	for (int k = 0; k < CYCLES; k++) {
		assert(reac_ring_write(&nr, nbuf, REAC_SAMPLES_PER_PKT, 2) == REAC_SAMPLES_PER_PKT);
		assert(reac_ring_read_planar(&nr, ndst, CH, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
			assert(fabsf(n0[s] - 77.0f) < 1e-6f);  /* rows still written */
			assert(n2[s] == 0.0f);                 /* retired on the shrink */
			assert(n3[s] == 0.0f);
		}
	}

	/* 3. A source claiming more rows than the ring holds is CLAMPED, not an
	 *    overflow. reac_upstream_channels is contract-bound below 40, but the ring
	 *    is the last line and must not lean on that. */
	assert(reac_ring_write(&nr, nbuf, REAC_SAMPLES_PER_PKT, CH + 99) == REAC_SAMPLES_PER_PKT);
	assert(reac_ring_read_planar(&nr, ndst, CH, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
		assert(fabsf(n3[s] - 77.0f) < 1e-6f);

	reac_ring_free(&nr);
	printf("test_reac_ring: OK\n");
	return 0;
}
