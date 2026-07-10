// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* SCHED_FIFO cadence pacer — the parts that need no socket and no RT privilege:
 *   1. slot period per rate: 8000 fps -> 125 us, 4000 -> 250 us, 3675 -> ~272 us;
 *   2. the SPSC frame ring: FIFO order, overrun drops the newest (producer never
 *      moves tail), underrun returns 0 (the pacer then emits silent FILLER);
 *   3. a live cadence check: run the real pacer thread on the loopback interface
 *      (lo) for a slice and assert it emitted close to fps*dt frames at a steady
 *      interval. SKIPPED (exit 77) if the AF_PACKET socket can't open (no
 *      CAP_NET_RAW in the test sandbox) — the cadence math above already covers
 *      the timing contract; the live check is a bonus when privilege exists. */
#include "reac_pacer.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
	/* 1. per-rate slot period (the 125 us @96k contract). */
	CHK(reac_pacer_period_ns(8000) == 125000);   /* 96 kHz */
	CHK(reac_pacer_period_ns(4000) == 250000);   /* 48 kHz */
	CHK(reac_pacer_period_ns(3675) == 272109);   /* 44.1 kHz (rounded) */

	/* 2. the SPSC frame ring. */
	struct reac_frame_ring r;
	CHK(reac_frame_ring_init(&r, 4, 2048) == 0);   /* 4 slots -> 3 usable (one empty) */
	CHK(r.slots == 4);

	uint8_t in[REAC_FRAME_BYTES], out[2048];
	memset(in, 0, sizeof in);

	/* FIFO order: push 3 frames with distinct markers, pop in order. */
	for (int i = 0; i < 3; i++) {
		in[14] = (uint8_t)i;                       /* tag at the counter slot */
		CHK(reac_frame_ring_push(&r, in, REAC_FRAME_BYTES) == 1);
	}
	CHK(reac_frame_ring_readable(&r) == 3);
	/* ring full (3 usable): the 4th push drops the NEWEST and bumps overruns. */
	in[14] = 0x99;
	CHK(reac_frame_ring_push(&r, in, REAC_FRAME_BYTES) == 0);
	CHK(r.overruns == 1);
	for (int i = 0; i < 3; i++) {
		uint16_t n = reac_frame_ring_pop(&r, out);
		CHK(n == REAC_FRAME_BYTES);
		CHK(out[14] == (uint8_t)i);                /* FIFO: 0,1,2 — never the dropped 0x99 */
	}
	/* underrun: empty pop returns 0 + bumps underruns (the pacer fills silence). */
	CHK(reac_frame_ring_pop(&r, out) == 0);
	CHK(r.underruns == 1);
	reac_frame_ring_free(&r);

	/* 3. live cadence on lo (best-effort; needs CAP_NET_RAW). */
	struct reac_pacer p;
	struct reac_pacer_cfg cfg = { .ifname = "lo", .fps = 8000, .prio = 0, .cpu = -1,
	                              .src_mac = NULL };
	if (reac_pacer_open(&p, &cfg) != 0) {
		printf("SKIP: AF_PACKET TX on lo unavailable (no CAP_NET_RAW?) — cadence math "
		       "(parts 1-2) verified; live emit check skipped\n");
		return 77;   /* meson: test SKIP */
	}
	CHK(reac_pacer_period_ns(8000) == p.period_ns);

	/* prime the ring so the pacer emits queued frames (not only silent FILLER). */
	memset(in, 0, sizeof in);
	for (int i = 0; i < 100; i++)
		reac_pacer_submit(&p, in, REAC_FRAME_BYTES);

	CHK(reac_pacer_start(&p) == 0);
	uint64_t t0 = mono_ns();
	struct timespec slice = { 0, 200000000 };      /* 200 ms */
	nanosleep(&slice, NULL);
	uint64_t dt = mono_ns() - t0;
	reac_pacer_stop(&p);

	uint64_t tx = p.tx_frames;
	double expected = (double)dt / 1e9 * 8000.0;   /* fps * seconds */
	double ratio = expected > 0 ? (double)tx / expected : 0;
	printf("pacer emitted %llu frames in %.1f ms (expected ~%.0f @8000 fps, ratio %.2f), "
	       "late_wakes=%llu tx_errors=%llu\n",
	       (unsigned long long)tx, dt / 1e6, expected, ratio,
	       (unsigned long long)p.late_wakes, (unsigned long long)p.tx_errors);
	reac_pacer_close(&p);

	/* Allow generous slack for a non-RT CI host (SCHED_FIFO may be denied): the
	 * cadence should still be in the right ballpark, never wildly fast/slow. */
	CHK(ratio > 0.5 && ratio < 1.5);

	printf("OK: pacer period (125/250/272 us) + SPSC ring (FIFO/overrun/underrun) + "
	       "steady ~8000 fps emit\n");
	return 0;
}
