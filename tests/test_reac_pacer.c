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
#include <reac/transport/reac_pacer.h>
#include <reac/reac_ctrl.h>
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

	/* 2b. depth-guard band derivation + trim math (task #152). The band comes from
	 * the ACTUAL producer burst (quantum/12 frames), not a guessed steady state:
	 * HIGH = max(FLOOR=512, MULT=4 * quantum_frames), TARGET = HIGH/2. */
	CHK(reac_pacer_guard_high(0) == REAC_PACER_GUARD_FLOOR_FRAMES);   /* no quantum yet -> floor */
	CHK(reac_pacer_guard_high(85) == REAC_PACER_GUARD_FLOOR_FRAMES);  /* 1024-sample quantum: 4*85=340 < 512 */
	CHK(reac_pacer_guard_high(128) == REAC_PACER_GUARD_FLOOR_FRAMES); /* 4*128=512 == floor */
	CHK(reac_pacer_guard_high(200) == 4u * 200u);                     /* 2400-sample quantum: burst term wins */

	/* Pure trim math: NEVER trims at the realistic live sawtooth peak (~113) or at
	 * k*quantum bursts; only genuine runaway drift above HIGH trims. */
	const uint32_t HI = 512, TG = 256;   /* the deployed band (floor governs) */
	CHK(reac_frame_ring_trim_count(0, HI, TG) == 0);
	CHK(reac_frame_ring_trim_count(38, HI, TG) == 0);      /* live sawtooth trough */
	CHK(reac_frame_ring_trim_count(113, HI, TG) == 0);     /* live sawtooth PEAK (measured) */
	CHK(reac_frame_ring_trim_count(340, HI, TG) == 0);     /* k*quantum pileup (4*85): still no trim */
	CHK(reac_frame_ring_trim_count(512, HI, TG) == 0);     /* exactly at high: still no trim */
	CHK(reac_frame_ring_trim_count(513, HI, TG) == 513 - TG);   /* one over: drain to target */
	CHK(reac_frame_ring_trim_count(1000, HI, TG) == 1000 - TG); /* runaway drift toward the cap */
	/* defensive: depth over a (mis)configured high but not over target -> no drop */
	CHK(reac_frame_ring_trim_count(300, 256, 400) == 0);

	/* the ring trim advances tail by exactly that many (SPSC: consumer owns tail). */
	struct reac_frame_ring g;
	CHK(reac_frame_ring_init(&g, 1024, 2048) == 0);        /* 1024 slots -> 1023 usable */
	uint8_t gf[REAC_FRAME_BYTES];
	memset(gf, 0, sizeof gf);
	for (int i = 0; i < 700; i++)                          /* fill to depth 700 (> high 512) */
		CHK(reac_frame_ring_push(&g, gf, REAC_FRAME_BYTES) == 1);
	CHK(reac_frame_ring_readable(&g) == 700);
	CHK(reac_frame_ring_trim(&g, HI, TG) == 700 - TG);     /* dropped 444 oldest */
	CHK(reac_frame_ring_readable(&g) == TG);               /* drained to target */
	CHK(reac_frame_ring_trim(&g, HI, TG) == 0);            /* now below high: no-op */
	CHK(reac_frame_ring_readable(&g) == TG);
	reac_frame_ring_free(&g);

	/* 2c. depth-telemetry gating (task #152, live-data fix): the depth SAWTOOTHS,
	 * so the line must fire ONLY on the heartbeat or a guard trim — NEVER on a
	 * depth change, or it spams every drain. First drain = baseline (heartbeat,
	 * log_last_ns==0); a second drain, even after a LARGE depth swing but no trim,
	 * must stay SILENT; a guard trim forces a line every time it fires. */
	{
		struct reac_pacer pg;
		memset(&pg, 0, sizeof pg);
		pg.handle = NULL;
		pg.fps = 4000;
		pg.period_ns = reac_pacer_period_ns(4000);
		atomic_store(&pg.ring_depth_min, UINT32_MAX);
		CHK(reac_frame_ring_init(&pg.ring, 2048, 2048) == 0);
		uint8_t gpf[REAC_FRAME_BYTES];
		memset(gpf, 0, sizeof gpf);
		for (int i = 0; i < 38; i++)                       /* live sawtooth trough */
			CHK(reac_frame_ring_push(&pg.ring, gpf, REAC_FRAME_BYTES) == 1);

		FILE *gs = tmpfile();
		CHK(gs != NULL);
		reac_pacer_log_drain(&pg, gs);                     /* baseline (heartbeat) */
		long after_first = ftell(gs);
		CHK(after_first > 0);                              /* one depth line emitted */

		/* sawtooth up to the ~113 peak: a big depth change but NO trim (< HIGH) and
		 * within the heartbeat window -> the drain MUST stay silent. */
		for (int i = 0; i < 75; i++)
			reac_frame_ring_push(&pg.ring, gpf, REAC_FRAME_BYTES);
		CHK(reac_frame_ring_readable(&pg.ring) == 113);
		reac_pacer_log_drain(&pg, gs);
		CHK(ftell(gs) == after_first);                     /* silent despite the swing */

		/* a guard trim forces a line even back-to-back. */
		for (int i = 0; i < 500; i++)                      /* drive depth over HIGH (512) */
			reac_frame_ring_push(&pg.ring, gpf, REAC_FRAME_BYTES);
		CHK(reac_frame_ring_trim(&pg.ring, reac_pacer_guard_high(0),
		                         reac_pacer_guard_high(0) / 2) > 0);
		atomic_fetch_add(&pg.ring_trims, 1);               /* pacer loop bumps this on a trim */
		reac_pacer_log_drain(&pg, gs);
		CHK(ftell(gs) > after_first);                      /* trim forced a line */
		fclose(gs);
		reac_frame_ring_free(&pg.ring);
	}

	/* 3. the RX ingest path WITHOUT a socket: construct the pacer by hand
	 * (frame ring + master FSM only, fd = -1) and feed fixture frames through
	 * reac_pacer_rx_ingest — counters, the fsm_state mirror, the event ring. */
	{
		static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
		struct reac_pacer p3;
		memset(&p3, 0, sizeof p3);
		p3.handle = NULL;
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
		bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, 2, 16, NULL, 12);
		uint8_t join_blk[32];
		memcpy(join_blk, bf + 18, 32);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.rx_joins == 1 && p3.rx_box_ctrl == 1);
		/* The JOIN is HELD until the scene push completes (reac_master.c): the box
		 * has only a partial scene until the final chunk, and granting into that is
		 * what left it in reassembly for the life of the link. Drive the cadence to
		 * the end of one transfer and the held edge is taken. */
		CHK(p3.master.state == REAC_M_PROBING);
		CHK(p3.master.join_held == 1);
		{
			uint16_t c; int ix;
			long guard = 0;
			while ((p3.master.scene_complete == 0 || p3.master.scene_inflight) &&
			       guard++ < 4L * p3.master.cycle_len)
				(void)reac_master_next(&p3.master, &c, &ix);
			CHK(p3.master.scene_complete == 1);
		}
		p3.fsm_state = p3.master.state;
		CHK(p3.fsm_state == REAC_M_GRANTING);
		CHK(p3.master.grant_attempts == 1);

		/* SELF-CONFIGURATION FROM THE WIRE, on the real ingest path (2026-08-05).
		 * The cold-connect above carries no width, so at this instant the master
		 * knows a box is courting it and nothing else — no allocation, no sweep, and
		 * no fabricated fallback to grant instead. What fills that in is the box's
		 * own config-announce, recognized here by reac_ctrl_identify_box against the
		 * fixed matrix, exactly as it happens on the wire. Nothing configured this;
		 * nothing could have. */
		CHK(reac_master_has_box(&p3.master) == 0);
		CHK(p3.master.grant_burst_len == 0);
		CHK(atomic_load(&p3.recognized_box) == NULL);

		bn = reac_ctrl_build_config_announce(bf, OUR, BOX, 3, 16);
		reac_pacer_rx_ingest(&p3, bf, bn);
		const struct reac_box_model *rec = atomic_load(&p3.recognized_box);
		CHK(rec != NULL && rec->in_ch == 16);
		CHK(reac_master_has_box(&p3.master) == 1);
		/* THE VALUE, not the shape: an S-1608's inputs are enrolled at head-amp
		 * 0x20..0x2f, and the enrollment that will reach the wire says so in every
		 * group-A record. This is the agreement head-amp control depends on. */
		CHK(p3.master.alloc.base == 0x20 && p3.master.alloc.width == 16);
		CHK(p3.master.grant_burst_len == 56);
		for (int i = 0; i < p3.master.grant_burst_len; i++) {
			const uint8_t *r = p3.master.grant_burst[i];
			if (!(r[16] == 0x12 && r[17] == 0x12 && r[18] == 0x01 && r[19] == 0x01))
				continue;                       /* not a group-A head-amp record */
			CHK(r[20] >= 0x20 && r[20] <= 0x2f);
		}

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

		/* the emit loop delivers ENROLL + the full 32-frame grant burst, then the
		 * master SELF-COMPLETES to ESTABLISHED and holds (#130 rig fix 2026-07-12:
		 * the box goes quiet after the grant, so waiting for a post-burst unicast
		 * made it re-attempt forever). Self-complete happens inside reac_master_next,
		 * so check the real FSM state (the p3.fsm_state mirror only advances on RX). */
		uint16_t ec; int ei;
		/* +grant_dwell: the ENROLL->grant dwell (~1.6 s, matching the measured
		 * M-200 gap — see grant_dwell's comment in reac_master_init) that now
		 * precedes the burst. */
		for (int i = 0; i < p3.master.grant_dwell + p3.master.grant_burst_len * p3.master.grant_stride + 2; i++)
			reac_master_next(&p3.master, &ec, &ei);
		CHK(p3.master.state == REAC_M_ESTABLISHED);

		/* the box heartbeat confirms the lock (mirror path). */
		bn = reac_ctrl_build_box_hb(bf, OUR, BOX, 3, 16);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.master.state == REAC_M_ESTABLISHED);

		/* THE PUBLISHED BOX IS A MIRROR, NOT A PARALLEL TRUTH. On a drop the master
		 * forgets the box, and what we publish has to go with it: reac.box-model /
		 * reac.box-width are what a consumer computes a head-amp address from, so a
		 * model left standing after the box has left is the console asserting a box
		 * that is not there. Then a re-join re-derives it from the wire, as always. */
		bn = reac_ctrl_build_box_hb(bf, OUR, BOX, 4, 16);
		bf[22] = 0x00;                            /* selector 0x00 = the box's BYE */
		reac_ctrl_checksum_apply(bf);             /* a corrupt block is not a BYE */
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(p3.master.state == REAC_M_PROBING);
		CHK(reac_master_has_box(&p3.master) == 0);
		CHK(atomic_load(&p3.recognized_box) == NULL);

		bn = reac_ctrl_build_config_announce(bf, OUR, BOX, 5, 16);
		reac_pacer_rx_ingest(&p3, bf, bn);
		CHK(atomic_load(&p3.recognized_box) != NULL);
		CHK(p3.master.alloc.base == 0x20 && p3.master.alloc.width == 16);

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
			bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, (uint16_t)i, 16, NULL, 12);
			reac_pacer_rx_ingest(&p3, bf, bn);
		}
		CHK(p3.ev_drops > 0);
		sink = tmpfile();
		CHK(sink != NULL);
		CHK(reac_pacer_log_drain(&p3, sink) == REAC_PACER_EVRING);
		fclose(sink);

		reac_frame_ring_free(&p3.ring);
	}

	/* 3a. THE IDENTITY PAGE on the ingest path: a box answers the grant sweep's
	 * identity poll (DT1 tag 0x0500) with single-record replies, and rx_ingest
	 * folds them into rx_identity BEFORE the FSM filter drops them as an unknown
	 * link-4 record. reac_pacer_read_identity lifts a consistent snapshot across
	 * the seqlock. Values, not shapes — the firmware digits are the S-0808's own. */
	{
		static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
		struct reac_pacer p4;
		memset(&p4, 0, sizeof p4);
		p4.handle = NULL;
		p4.fps = 8000;
		memcpy(p4.src, OUR, 6);
		CHK(reac_frame_ring_init(&p4.ring, 8, 2048) == 0);
		reac_master_init(&p4.master, OUR, NULL, 8000);

		struct reac_identity id0;
		reac_pacer_read_identity(&p4, &id0);
		CHK(id0.has_fw == 0 && id0.has_reac_version == 0);   /* nothing answered yet */

		/* Lay a DT1 identity reply into a frame: 88 19 / type cd ea / control block
		 * whose SysEx is f0 41 0a 00 00 12 12 <tag> <addr_lo> <payload> <ck> f7. */
		uint8_t frame[REAC_FRAME_BYTES];
		#define BUILD_ID_REPLY(FR, ADDR, ...) do { \
			const uint8_t _pl[] = { __VA_ARGS__ }; \
			size_t _n = sizeof _pl; \
			memset((FR), 0, REAC_FRAME_BYTES); \
			memcpy((FR), OUR, 6); memcpy((FR) + 6, BOX, 6); \
			(FR)[12] = 0x88; (FR)[13] = 0x19; (FR)[16] = 0xcd; (FR)[17] = 0xea; \
			uint8_t *_b = (FR) + 18; unsigned _sx = (unsigned)(13 + _n); \
			_b[0] = 0x04; _b[1] = 0x03; _b[3] = (uint8_t)(_sx + 5); \
			_b[5] = 0x02; _b[7] = 0xfe; _b[8] = (uint8_t)_sx; \
			_b[9] = 0xf0; _b[10] = 0x41; _b[11] = 0x0a; _b[14] = 0x12; _b[15] = 0x12; \
			_b[16] = 0x05; _b[17] = 0x00; \
			_b[18] = (uint8_t)((ADDR) >> 8); _b[19] = (uint8_t)((ADDR) & 0xff); \
			for (size_t _i = 0; _i < _n; _i++) _b[20 + _i] = _pl[_i]; \
			_b[20 + _n] = 0x7f; _b[21 + _n] = 0xf7; \
		} while (0)

		/* firmware addr 0x0000: 01 00 00 03 -> 1.003 */
		BUILD_ID_REPLY(frame, REAC_IDENTITY_ADDR_FIRMWARE, 0x01, 0x00, 0x00, 0x03);
		reac_pacer_rx_ingest(&p4, frame, REAC_FRAME_BYTES);
		/* REAC version addr 0x0600: the S-0808's eight bytes, (0,1,0,0) */
		BUILD_ID_REPLY(frame, REAC_IDENTITY_ADDR_REAC_VERSION, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00);
		reac_pacer_rx_ingest(&p4, frame, REAC_FRAME_BYTES);

		struct reac_identity id1;
		reac_pacer_read_identity(&p4, &id1);
		CHK(id1.has_fw == 1 && id1.fw_milli == 1003);
		CHK(id1.has_reac_version == 1);
		CHK(id1.reac_version_major == 1 && id1.reac_version_minor == 0 &&
		    id1.reac_version_patch == 0);
		char fw[REAC_IDENTITY_FW_STR_CAP];
		CHK(reac_identity_fw_str(id1.fw_milli, fw, sizeof fw) == 5 && strcmp(fw, "1.003") == 0);
		/* The firmware and the REAC version are DIFFERENT numbers off DIFFERENT
		 * addresses: this box runs firmware 1.003 and speaks REAC 1.000. (The
		 * S-0808's REAC string is PREDICTED — the console displays read on
		 * 2026-09-14 were an S-1608 and an S-4000S-3208.) */
		char ver[REAC_IDENTITY_REAC_VER_STR_CAP];
		CHK(reac_identity_reac_ver_str(id1.reac_version_major, id1.reac_version_minor,
		                               id1.reac_version_patch, ver, sizeof ver) == 5);
		CHK(strcmp(ver, "1.000") == 0 && strcmp(ver, fw) != 0);

		#undef BUILD_ID_REPLY
		reac_frame_ring_free(&p4.ring);
	}

	/* 3b. DYNAMIC DETECTION: geometry comes from the box's DECLARATION — the
	 * config-announce port table (libreac reac_ports_parse) — and the matrix
	 * only NAMES the model. An unnamed
	 * 0x84-family variant (a tail byte no matrix row carries, table intact)
	 * must still size the master: set_box from the declared 32x8, ENROLL and
	 * the cfea width byte follow, while the model stays honestly unnamed. */
	{
		static const uint8_t OUR[6]  = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		static const uint8_t BOX2[6] = { 0x00, 0x40, 0xab, 0xc4, 0x99, 0x99 };
		struct reac_pacer p5;
		memset(&p5, 0, sizeof p5);
		p5.handle = NULL;
		p5.fps = 8000;
		memcpy(p5.src, OUR, 6);
		CHK(reac_frame_ring_init(&p5.ring, 8, 2048) == 0);
		reac_master_init(&p5.master, OUR, NULL, 8000);
		p5.prev_state = REAC_M_IDLE;

		uint8_t bf[2048];
		/* the matrix S-4000S announce (32 in), then a tail byte no row carries —
		 * the port table at block[8..19] is untouched, so the DECLARATION still
		 * reads 32x8 while reac_ctrl_identify_box has no byte-exact match. */
		size_t bn = reac_ctrl_build_config_announce(bf, OUR, BOX2, 7, 32);
		CHK(bn > 0);
		bf[18 + 26] ^= 0x5a;                      /* block[26]: model tail data */
		reac_ctrl_checksum_apply(bf);             /* keep the frame VALID */
		CHK(reac_ctrl_identify_box(bf, bn) == NULL);   /* no row names it */

		reac_pacer_rx_ingest(&p5, bf, bn);
		CHK(atomic_load(&p5.recognized_box) == NULL);  /* honestly unnamed... */
		CHK(reac_master_has_box(&p5.master) == 1);     /* ...but SIZED */
		CHK(p5.master.alloc.width == 32);
		CHK(p5.master.announce_blk[18] == 32);         /* cfea width byte = declared */

		reac_frame_ring_free(&p5.ring);
	}

	/* cfea[19] is the PACE CODE the box follows, not the console family (measured
	 * 2026-09-11 on one M-200: 0x00 at 48 k, 0x02 at 44.1 k; the M-5000 corpus 0x01 at
	 * 96 k). Before this mapping our 44.1 k master announced the 48 k code and an
	 * S-4000S returned 4000 pps under a 3675 pps cadence. Pin the mapping and the byte
	 * placement without a socket. */
	{
		CHK(reac_pace_code(3675) == 2);   /* 44.1 kHz */
		CHK(reac_pace_code(4000) == 0);   /* 48 kHz */
		CHK(reac_pace_code(8000) == 1);   /* 96 kHz */
		static const uint8_t OUR[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		struct reac_console_cfg cfg = { .out_channels = 16, .console_field = reac_pace_code(3675) };
		struct reac_master m;
		reac_master_init(&m, OUR, &cfg, 3675);
		CHK(m.announce_blk[19] == 2);     /* the byte a box paces by */
	}

	/* ---- the sustained-discard detector --------------------------------- *
	 * Its predecessor counted CONSECUTIVE trimming drains and needed twenty. On
	 * the rig the guard trims once every ~24 s against a 200 ms drain cadence, so
	 * the run reset to zero between every trim and the warning NEVER FIRED while
	 * 64 ms of audio went missing every 24 seconds -- 356 trims, 0 warnings,
	 * confirmed with a positive control on the same log. The first case below is
	 * that exact cadence, and it is the case the old detector failed.
	 *
	 * Time and the trim counter are both parameters, so this runs in microseconds
	 * and needs no pacer, no socket and no sleeping. */
	{
		const uint64_t W = REAC_PACER_DISCARD_WIN_NS;
		struct reac_discard_watch w = { 0 };
		double rate = 0;
		uint64_t t = 1000000000ull, frames = 0;

		/* THE RIG'S CADENCE: one 258-frame trim every 24 s, i.e. every window
		 * discards something but no two drains ever trim back to back. */
		CHK(reac_discard_watch_step(&w, t, frames, &rate) == 0);   /* opens */
		int fired = 0, fires = 0;
		for (int win = 0; win < 6; win++) {
			t += W;
			frames += 258;                       /* one trim inside this window */
			if (reac_discard_watch_step(&w, t, frames, &rate)) {
				fires++;
				if (!fired) {
					fired = 1;
					/* 258 frames over a 30 s window */
					CHK(rate > 8.0 && rate < 9.0);
					CHK(w.run == REAC_PACER_DISCARD_WIN_RUN);
				}
			}
		}
		CHK(fired == 1);        /* it fired */
		CHK(fires == 1);        /* and exactly once, not every window after */

		/* A ONE-OFF must stay quiet. One trim, then clean windows: a scene recall
		 * or a startup transient is not a rate mismatch and must not cry wolf. */
		struct reac_discard_watch q = { 0 };
		uint64_t qt = 5000000000ull, qf = 0;
		CHK(reac_discard_watch_step(&q, qt, qf, &rate) == 0);
		qt += W; qf += 300;                      /* the one-off */
		CHK(reac_discard_watch_step(&q, qt, qf, &rate) == 0);
		for (int win = 0; win < 10; win++) {     /* silence afterwards */
			qt += W;
			CHK(reac_discard_watch_step(&q, qt, qf, &rate) == 0);
		}
		CHK(q.run == 0);        /* a clean window broke the run */
		CHK(q.warned == 0);

		/* AN INTERRUPTED run must not accumulate across the gap: two discarding
		 * windows, one clean, then two more discarding is five windows of which
		 * no three are consecutive, and must stay silent. */
		struct reac_discard_watch r = { 0 };
		uint64_t rt = 9000000000ull, rf = 0;
		CHK(reac_discard_watch_step(&r, rt, rf, &rate) == 0);
		const int pattern[5] = { 1, 1, 0, 1, 1 };
		for (int i = 0; i < 5; i++) {
			rt += W;
			if (pattern[i])
				rf += 100;
			CHK(reac_discard_watch_step(&r, rt, rf, &rate) == 0);
		}
		CHK(r.warned == 0);

		/* A window that has NOT closed yet decides nothing, however much was
		 * discarded inside it. */
		struct reac_discard_watch h = { 0 };
		uint64_t ht = 1000000ull;
		CHK(reac_discard_watch_step(&h, ht, 0, &rate) == 0);
		CHK(reac_discard_watch_step(&h, ht + W - 1, 999999, &rate) == 0);
		CHK(h.run == 0);
	}

	/* ---- the guard-floor override --------------------------------------- *
	 * The M5 sweep sets REACPW_GUARD_FLOOR_FRAMES. A value it cannot parse, or one
	 * outside the safe band, must keep the COMPILED floor -- a sweep step that
	 * silently ran at the default would attribute a whole row of measurements to a
	 * depth that was never configured. reac_pacer_guard_floor resolves once per
	 * process, so this checks the pure combination rule around it. */
	CHK(reac_pacer_guard_high(0) >= REAC_PACER_GUARD_FLOOR_MIN);
	CHK(reac_pacer_guard_high(1000) == REAC_PACER_GUARD_BURST_MULT * 1000);
	CHK(reac_pacer_guard_high(1) == reac_pacer_guard_floor());

	/* 4. live cadence on lo (best-effort; needs CAP_NET_RAW). */
	struct reac_pacer p;
	struct reac_pacer_cfg cfg = { .ifname = "lo", .fps = 8000, .prio = 0, .cpu = -1,
	                              .src_mac = NULL };
	if (reac_pacer_open(&p, &cfg) != 0) {
		/* Name what RAN. "parts 1-2" undersold it by five sections and made a
		 * SKIP line read as though almost nothing had been checked — everything
		 * up to here needs no socket and has already asserted. Only part 4, the
		 * live emit on lo, is skipped. */
		printf("SKIP: AF_PACKET TX on lo unavailable (no CAP_NET_RAW?) — parts 1, 2, "
		       "2b, 2c (slot period, SPSC ring, depth-guard band, depth telemetry), "
		       "3, 3a, 3b (RX ingest, identity page, dynamic detection) and the "
		       "discard watch + guard-floor rules all RAN and passed; only part 4, "
		       "the live emit on lo, is skipped\n");
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
	       "steady ~8000 fps emit + sustained-discard detector + guard floor\n");
	return 0;
}
