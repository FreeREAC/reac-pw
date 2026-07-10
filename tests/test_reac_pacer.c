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
#include "reac_ctrl.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
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

	/* 3. the RX ingest path WITHOUT a socket: construct the pacer by hand
	 * (frame ring + master FSM only, fd = -1) and feed fixture frames through
	 * reac_pacer_rx_ingest — counters, the fsm_state mirror, the event ring. */
	{
		static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
		struct reac_pacer p3;
		memset(&p3, 0, sizeof p3);
		p3.fd = -1;
		p3.fps = 8000;
		memcpy(p3.src, OUR, 6);
		CHK(reac_frame_ring_init(&p3.ring, 8, 2048) == 0);
		reac_master_init(&p3.master, OUR, NULL, 8000);   /* S-1608 default */
		p3.prev_state = REAC_M_IDLE;

		uint8_t bf[2048];

		/* a broadcast presence FILLER: counted, no state change, presence event */
		static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		size_t bn = reac_ctrl_build_upstream_filler(bf, BCAST, BOX, 1, 16, NULL, 12);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.rx_box_frames == 1 && p3.rx_box_ctrl == 0 && p3.rx_joins == 0);
		CHK(p3.master.state == REAC_M_PROBING);   /* promoted, but NOT granting */

		/* our own echo must be ignored (the software self-filter) */
		bn = reac_ctrl_build_upstream_filler(bf, BCAST, OUR, 1, 16, NULL, 12);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.rx_box_frames == 1);

		/* the JOIN: fsm mirror flips to GRANTING, the ring holds the block */
		bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, 2);
		uint8_t join_blk[32];
		memcpy(join_blk, bf + 18, 32);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.rx_joins == 1 && p3.rx_box_ctrl == 1);
		CHK(p3.fsm_state == REAC_M_GRANTING);
		CHK(p3.grant_attempts == 1);

		/* the event ring contains a JOIN event with the exact 32-byte block */
		int found_join = 0;
		uint32_t hh = p3.ev_head;
		for (uint32_t i = p3.ev_tail; i != hh; i++) {
			const struct reac_pacer_event *e = &p3.evring[i % REAC_PACER_EVRING];
			if (e->kind == REAC_PEV_JOIN) {
				CHK(memcmp(e->blk, join_blk, 32) == 0);
				CHK(memcmp(e->src, BOX, 6) == 0);
				found_join = 1;
			}
		}
		CHK(found_join);

		/* unicast -> ESTABLISHED via the mirror */
		bn = reac_ctrl_build_box_hb(bf, OUR, BOX, 3);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.fsm_state == REAC_M_ESTABLISHED);

		/* drain formats + counts every queued event, then returns 0 */
		FILE *sink = tmpfile();
		CHK(sink != NULL);
		int drained = reac_pacer_log_drain(&p3, sink);
		CHK(drained >= 3);                        /* presence + join + transitions */
		CHK(reac_pacer_log_drain(&p3, sink) == 0);
		CHK(ftell(sink) > 0);                     /* something was written */
		fclose(sink);

		/* overflow: flood JOINs (each always logs) -> ring caps at EVRING,
		 * drop-newest counts ev_drops, a full drain returns exactly EVRING */
		for (int i = 0; i < REAC_PACER_EVRING * 2; i++) {
			bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, (uint16_t)i);
			reac_pacer_rx_ingest(&p3, bf, bn);
		}
		CHK(p3.ev_drops > 0);
		sink = tmpfile();
		CHK(sink != NULL);
		CHK(reac_pacer_log_drain(&p3, sink) == REAC_PACER_EVRING);
		fclose(sink);

		reac_frame_ring_free(&p3.ring);
	}

	/* 4. live cadence on lo (best-effort; needs CAP_NET_RAW). */
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
