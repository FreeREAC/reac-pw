// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* MASTER-role handshake: byte oracle + the EVENT-DRIVEN establishment (#130).
 *
 * The master parameterizes its downstream from a console I/O config; fed the
 * S-1608 config it reproduces the captured M-300 downstream byte-for-byte
 * (reac-captures/m300-s1608-*.pcap, real master 00:40:ab:c9:d8:5b).
 *
 * Byte oracle:
 *   1. the generated channel-map frame byte-matches the captured M-300 block
 *      (no MAC in the chanmap -> EXACT) AND Sum(block[18..49]) mod 256 == 0;
 *   2. the generated cfea = fixed head + OUR MAC + inCh 0x28 + outCh + console
 *      field + checksum (identity fix: on-wire identity must match the L2 src);
 *   3. the grant ECHOES the received JOIN block verbatim;
 *   4. PROBE / SUB01 / SUB02 are the fixed M-300 protocol constants, byte-exact;
 *   5. the (single-frame) chanmap covers channels 0x00..0x06 after the marker.
 *
 * FSM (the anti-#130 core): NO timer ever advances toward ESTABLISHED —
 *   a. no-RX soak: 60 s of slots with zero RX events stays PROBING, zero grants,
 *      the full continuous 5-message cadence flows (PROBE ~115/s + the four 1/s);
 *   b. the golden response sequence: presence never grants; JOIN -> GRANTING
 *      (echo burst at 1-per-12 density); first box unicast -> ESTABLISHED; the
 *      same continuous cadence runs while linked;
 *   c. safety fallbacks only move BACKWARD: grant-window expiry, 600-frame
 *      peer-gone budget, explicit BYE, box-MAC change;
 *   d. the counter free-runs monotonically (mod 2^16) across every transition. */
#include <reac/reac_master.h>
#include <reac/reac_ctrl.h>
#include <reac/transport/reac_tx.h>
#include <reac/reac_decode.h>
#include <reac/reac.h>
#include <reac/reac_encode.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* THE BASE A BOX ANNOUNCES, not one derived from its width. These are the
 * straps the real chassis carry in their config announce (block[7] * 0x10,
 * libreac reac_ports.h): an S-1608 straps 2, an S-0808 straps 0. They are
 * written out here rather than computed from in_ch on purpose — a helper
 * mapping width to base is the very table this law retired, and it would agree
 * with the wire on exactly the chassis we own. */
#define S1608_BASE 0x20
#define S0808_BASE 0x00

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* The captured M-300/S-1608 chanmap marker window and cfea announce, bytes
 * [16:50] (type[2] + block[32]) — the master MUST stamp exactly these, with our
 * MAC substituted into the cfea. ONE copy, shared with test_reac_s1608.c. */
#include "reac_m300_golden.inc"
/* The header and the final chunk of the scene push, transcribed off a real desk
 * LONG BEFORE the transfer was understood — which is what makes them an oracle
 * here: the chunker must reproduce both from the recovered body alone. */
static const uint8_t GOLD_SUB01[34] =
 { 0xcd,0xea,0x01,0x01,0x00,0x18,0x00,0x22,0xc8,0x31,0x32,0x33,0x34,0x01,0x00,0x00,0x00,0x04,0x00,0x01,0x80,0x02,0x00,0x01,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xa7 };
static const uint8_t GOLD_SUB02[34] =
 { 0xcd,0xea,0x01,0x02,0x00,0x0e,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xe7 };

/* The byte-verified zoneA-48k S-1608 cold-connect block (what a real box sends
 * as its JOIN; the master must echo it verbatim as the grant). */
static const uint8_t ZONEA_JOIN[32] = {
 0x04,0x03,0x00,0x14,0x00,0x02,0x00,0xfe,0x0f,0xf0,0x41,0x0a,0x00,0x00,0x12,0x12,
 0x01,0x00,0x06,0x00,0x01,0x00,0x78,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const uint8_t SRC[6]  = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
static const uint8_t BOX[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
static const uint8_t BOX2[6] = { 0x00, 0x40, 0xab, 0x09, 0x09, 0x09 };

/* The FILLER descriptor is 16x "00 <checksum of the CURRENT probe>" (#130): it
 * tracks the rotating probe, so the test compares against m.filler_desc rather
 * than any fixed byte — a frozen descriptor is exactly the bug we fixed. */

#define FPS 8000

/* Build a base downstream FILLER (so the audio + tail are present), then let the
 * master stamp the control block, and return the frame in `out`. */
static void build_and_stamp(const struct reac_master *m, uint8_t *out,
                            enum reac_master_emit emit, int idx,
                            float *const *planar)
{
	reac_downstream_build(out, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, SRC);
	reac_master_stamp(m, out, emit, idx);
}

/* One slot: advance the FSM + return the emit kind; checks the counter
 * free-runs by exactly +1 (mod 2^16) on EVERY slot, across all transitions. */
static enum reac_master_emit slot(struct reac_master *m, int *idx,
                                  uint16_t *expect_counter)
{
	uint16_t c;
	int i;
	enum reac_master_emit e = reac_master_next(m, &c, &i);
	if (c != *expect_counter) {
		fprintf(stderr, "FAIL: counter %u != expected %u\n", c, *expect_counter);
		return (enum reac_master_emit)-1;
	}
	(*expect_counter)++;
	if (idx)
		*idx = i;
	return e;
}

/* Drive the cadence until one whole scene push has been delivered. A real master
 * has been pushing its scene for cycles before a box ever answers; ours holds any
 * forward edge until the transfer completes (reac_master.c), so a test that wants
 * to reach GRANTING has to put the master in that state honestly. */
static void deliver_scene(struct reac_master *m, uint16_t *expect_counter)
{
	uint16_t c;
	int ix;
	long guard = 0;
	/* Not merely "one transfer done": the master must also be BETWEEN transfers.
	 * A JOIN that lands mid-push is held until that push finishes, so a test
	 * wanting an immediate grant has to stand in the quiet window the way a box
	 * that joins between two transfers does. */
	while ((m->scene_complete == 0 || m->scene_inflight) &&
	       guard++ < 4L * m->cycle_len)
		(void)reac_master_next(m, &c, &ix);
	/* The counter free-runs one per slot; resync the caller's tracker to it so
	 * slot()'s +1-per-slot invariant still holds across the transfer. */
	if (expect_counter)
		*expect_counter = m->counter;
}

/* Drive the master from a JOIN to ESTABLISHED the way a real box does: the JOIN
 * opens GRANTING, the emit loop delivers the FULL 32-frame grant burst, then the
 * box's unicast accept lands. The accept is gated on burst completion (#130 rig
 * fix 2026-07-12: a warm-relink box unicasts immediately and would otherwise cut
 * the burst to ~1 frame, leaving the box's light blinking). */
static void establish(struct reac_master *m, uint16_t *cnt, const uint8_t box[6])
{
	deliver_scene(m, cnt);   /* the push completes before the grant, as on the wire */
	reac_master_rx(m, REAC_M_RX_BOX_JOIN, box, ZONEA_JOIN);
	/* The box declares WHAT IT IS — its config-announce, which reac_pacer turns into
	 * this call. Nothing can be enrolled before it: the cold-connect JOIN carries no
	 * width and the master no longer holds a fabricated one to fall back on. Every
	 * re-join re-declares, because a drop forgets the box (reac_master_forget_box). */
	reac_master_set_box(m, 16, 8, S1608_BASE);
	/* +1 for the leading ENROLL slot, +grant_dwell for the ENROLL->grant dwell
	 * (~1.6 s, matching the measured M-200 gap), before the 32-block burst. */
	for (int i = 0; i < m->grant_dwell + m->grant_burst_len * REAC_M_GRANT_STRIDE + 1; i++)
		slot(m, NULL, cnt);
	reac_master_rx(m, REAC_M_RX_BOX_UNICAST, box, NULL);
}

int main(void)
{
	/* planar audio for the FILLER-survives test */
	float chbuf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		planar[ch] = chbuf[ch];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			chbuf[ch][s] = (ch == 0)
				? 0.5f * sinf(2.0f * (float)M_PI * (float)s / 12.0f)
				: (float)ch / 64.0f - 0.3f;
	}

	uint8_t f[REAC_FRAME_BYTES];
	struct reac_master m;
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	reac_master_init(&m, SRC, &idle, FPS);

	/* The master starts knowing NOTHING about any box (2026-08-05): no allocation,
	 * no sweep, and the IDLE cfea below is what it announces about itself. Sections
	 * that need a box DECLARE one, because on the wire the box is what declares it. */
	CHK(reac_master_has_box(&m) == 0);
	CHK(m.alloc.width == 0 && m.grant_burst_len == 0);

	/* the downstream chanmap is the 11-window fabric sweep (#130); window 0 is the
	 * fe frame (marker + 0x00..0x06), asserted below against M300_CHANMAP_FE. */
	CHK(m.chanmap_nframes == 49);

	/* ---- byte oracle ---------------------------------------------------- */

	/* 1. the generated channel-map frame byte-matches the captured M-300 block
	 * (no MAC in the chanmap -> EXACT) + checksum. */
	build_and_stamp(&m, f, REAC_M_EMIT_CHANMAP, 0, planar);
	CHK(memcmp(f + 16, M300_CHANMAP_FE, 34) == 0);      /* type + block exact */
	CHK(reac_ctrl_checksum_verify(f) == 0);          /* Sum[18..49]%256==0 */
	CHK(f[16] == 0xcd && f[17] == 0xea);             /* cdea */
	CHK(f[18] == 0x01 && f[19] == 0x03);             /* established sub-state */
	CHK(f[20] == 0x00 && f[21] == 0x19);             /* BE len 0x0019 */
	CHK(f[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 &&
	    f[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);

	/* 2. cfea = fixed head + OUR MAC + inCh 0x28 + outCh 0x08 + console 0 +
	 * recomputed checksum (identity fix — NEVER a cloned desk MAC). */
	build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
	CHK(f[16] == 0xcf && f[17] == 0xea);                 /* cfea */
	CHK(memcmp(f + 16, M300_CFEA, M300_CFEA_MAC_IDX) == 0); /* head intact */
	CHK(memcmp(f + 16 + M300_CFEA_MAC_IDX, SRC, 6) == 0); /* OUR MAC embedded */
	CHK(memcmp(f + 16 + M300_CFEA_MAC_IDX + 6,           /* tail intact (pre-cksum) */
	           M300_CFEA + M300_CFEA_MAC_IDX + 6, 34 - M300_CFEA_MAC_IDX - 6 - 1) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[16 + 17] == 0x28 && f[16 + 18] == 0x08);       /* inCh 40, outCh 8 */

	/* 2b. reac_master_set_box carries the RECOGNIZED box's INPUT width into the
	 * cfea announce (deviation fix, byte-cited vs real M-200 goldens): block byte
	 * 16 = frame offset 34 = out[18] was left STATIC at the init-time default
	 * regardless of the box that actually linked. A real M-200 emits 0x10 while
	 * driving a 16-in S-1608 and 0x08 for an 8-in S-0808 (gen_cfea's block
	 * comment); set_box now re-stamps announce_blk (gen_cfea + checksum)
	 * immediately at recognition time, before any grant/RX event. */
	{
		struct reac_master mw;
		reac_master_init(&mw, SRC, &idle, FPS);

		reac_master_set_box(&mw, 16, 8, S1608_BASE);             /* S-1608: 16 in / 8 out */
		build_and_stamp(&mw, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
		CHK(f[16 + 18] == 0x10);                     /* width byte tracks the box */
		CHK(reac_ctrl_checksum_verify(f) == 0);       /* re-stamped, still valid */

		reac_master_set_box(&mw, 8, 8, S0808_BASE);              /* S-0808: 8 in / 8 out */
		build_and_stamp(&mw, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
		CHK(f[16 + 18] == 0x08);
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* THE WIDTH TRACKS RECOGNITION, THE COUNT TRACKS THE GRANT — two different
		 * instants, measured ~2 s apart on an M-200 bouncing an S-1608 at 44.1 kHz
		 * (reac-captures/m200-enrol-441k-2026-09-13/analysis.md, timeline):
		 *
		 *   box absent           cfea width 0x08, count 0
		 *   box's commit report  width back to 0x10, count STILL 0
		 *   the grant burst
		 *   +0.5 s               count 0 -> 1
		 *
		 * So a recognition that lands mid-GRANTING — the cold path, during the dwell
		 * enter_granting exists to hold — must still announce count 0: nothing is
		 * enrolled until a grant has left the wire. The count rises at
		 * enter_established and nowhere else, and a later recognition while
		 * ESTABLISHED (a warm relink, a model change) keeps its 1. Both arms are
		 * asserted below. */
		/* THE PUSH RUNS TO COMPLETION BEFORE THE GRANT. A JOIN that lands before the
		 * box has the whole scene is HELD — not dropped, not honoured — and taken on
		 * the final chunk. Both halves of that law are asserted here, because it is
		 * the whole fix: the box JOINs in milliseconds against a 2.694 s cycle, so
		 * without the hold the transfer is cancelled on every establishment and the
		 * box never leaves reassembly. */
		CHK(reac_master_rx(&mw, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 0);
		CHK(mw.state == REAC_M_PROBING);          /* held, not advanced */
		CHK(mw.join_held == 1);
		{
			uint16_t c; int ix;
			long guard = 0;
			while (mw.scene_complete == 0 && guard++ < 4L * mw.cycle_len)
				(void)reac_master_next(&mw, &c, &ix);
			(void)0;
			CHK(mw.scene_complete == 1);          /* one whole transfer delivered */
			CHK(guard < 4L * mw.cycle_len);
		}
		CHK(mw.join_held == 0);                   /* released on the final chunk */
		CHK(mw.state == REAC_M_GRANTING);
		reac_master_set_box(&mw, 8, 8, S0808_BASE);              /* recognized mid-grant */
		build_and_stamp(&mw, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
		CHK(f[16 + 18] == 0x08);                       /* the width follows at once */
		CHK(f[16 + 20] == 0x00 && f[16 + 21] == 0x00); /* ungranted: count still 0 */
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* AND IT RISES WHEN THE GRANT HAS GONE OUT. Run the ENROLL->grant dwell
		 * and the whole 32-block burst: this master commits to ESTABLISHED on
		 * delivery rather than on a post-burst box unicast (reac_master.c's
		 * GRANT_DELIVERED edge), and the count is re-stamped there and nowhere
		 * else. Every slot of the dwell and the burst is checked, not just the
		 * ends — the window the M-200 announces as ungranted is the whole of it. */
		{
			uint16_t c; int ix;
			long budget = mw.grant_dwell +
			              (long)mw.grant_burst_len * REAC_M_GRANT_STRIDE + 8;
			while (mw.state == REAC_M_GRANTING && budget-- > 0) {
				(void)reac_master_next(&mw, &c, &ix);
				if (mw.state == REAC_M_GRANTING)
					CHK(mw.announce_blk[20] == 0x00 &&
					    mw.announce_blk[21] == 0x00);
			}
			CHK(budget > 0);                  /* the burst finished inside it */
		}
		CHK(mw.state == REAC_M_ESTABLISHED);
		build_and_stamp(&mw, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
		CHK(f[16 + 18] == 0x08);                       /* still the S-0808's width */
		CHK(f[16 + 20] == 0x00 && f[16 + 21] == 0x01); /* count rises at the grant */
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* a warm relink (a DIFFERENT model recognized while established) moves the
		 * width and keeps the 1 — the count must not fall back to the idle 0. */
		reac_master_set_box(&mw, 16, 8, S1608_BASE);
		build_and_stamp(&mw, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
		CHK(f[16 + 18] == 0x10);
		CHK(f[16 + 20] == 0x00 && f[16 + 21] == 0x01);
		CHK(reac_ctrl_checksum_verify(f) == 0);
	}

	/* The box declares itself (what reac_pacer does on the config-announce). Only
	 * now is there an enrollment to grant. */
	reac_master_set_box(&m, 16, 8, S1608_BASE);
	CHK(reac_master_has_box(&m) == 1);

	/* 3. the grant is the master's OWN burst sweep (byte-exact M-200 cdea 04 03),
	 * NOT an echo of the box's JOIN (the echo model was falsified by
	 * matrix-m200-s0808 2026-07-12 — a locked box gets the master's sweep). Block
	 * 0 of the S-0808 burst is a 04030014 frame. */
	deliver_scene(&m, NULL);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	build_and_stamp(&m, f, REAC_M_EMIT_GRANT, 0, planar);
	CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x04 && f[19] == 0x03);
	CHK(memcmp(f + 16, m.grant_burst[0], 34) == 0);      /* burst block 0, byte-exact */
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* 3b. The grant sweep is GENERATED from OUR allocation for the recognized box,
	 * and — the load-bearing property — the slots it enrolls are the slots our
	 * head-amp traffic later addresses. Replaying a captured S-0808 sweep at an
	 * S-1608 is exactly the 2026-07-17 live failure: the box links, then ignores
	 * every head-amp record for CH 0x20 because our grant never claimed it.
	 * Full sweep-shape/byte coverage lives in tests/test_reac_grant.c; here we pin
	 * that the MASTER wires the allocation through to the emitted grant. */
	{
		struct reac_master mg;

		reac_master_init(&mg, SRC, &idle, FPS);
		reac_master_set_box(&mg, 16, 8, S1608_BASE);                 /* a real S-1608 links */
		CHK(mg.alloc.base == 0x20 && mg.alloc.width == 16);
		CHK(mg.grant_burst_len == 56);                   /* 8 + 16*3 */

		reac_master_set_box(&mg, 8, 8, S0808_BASE);                  /* an S-0808 instead */
		CHK(mg.alloc.base == 0x00 && mg.alloc.width == 8);
		CHK(mg.grant_burst_len == 32);                   /* 8 + 8*3 — sweep RESIZED */

		/* Every group-A record the sweep emits addresses a slot inside the
		 * allocation. This is the invariant the replayed table violated. */
		reac_master_set_box(&mg, 16, 8, S1608_BASE);
		int groupa = 0;
		for (int i = 0; i < mg.grant_burst_len; i++) {
			const uint8_t *r = mg.grant_burst[i];
			if (!(r[16] == 0x12 && r[17] == 0x12 && r[18] == 0x01 && r[19] == 0x01))
				continue;
			groupa++;
			CHK(r[20] >= mg.alloc.base);
			CHK(r[20] < mg.alloc.base + mg.alloc.width);
		}
		CHK(groupa == 16 * 3);

		/* A width we cannot place must not HALF-apply: the allocation, the ENROLL
		 * group map and the cfea width byte move together or not at all. The
		 * previous cut let the allocation be refused and then stamped the bad width
		 * into the ENROLL and the announce anyway. */
		uint8_t enroll_before[34];
		memcpy(enroll_before, mg.enroll_blk, 34);
		uint8_t cfea_before = mg.cfg.out_channels;
		reac_master_set_box(&mg, 999, 8, S1608_BASE);                /* nonsense recognition */
		CHK(mg.grant_burst_len == 56);                   /* previous sweep retained */
		CHK(mg.alloc.base == 0x20 && mg.alloc.width == 16);
		CHK(memcmp(enroll_before, mg.enroll_blk, 34) == 0);
		CHK(mg.cfg.out_channels == cfea_before);
	}

	/* 4. The chunker reproduces the two transcribed constants from the body: step 0
	 * is the header (declaring 0x22c8 + the body's first 24 B) and the last step is
	 * the final chunk. Neither is a canned block any more — both are derived, so a
	 * body of the wrong length or a mis-sliced payload fails here. */
	CHK(reac_ctrl_build_scene_step(m.scene_blk, m.scene, sizeof m.scene, 0) == 0);
	build_and_stamp(&m, f, REAC_M_EMIT_SCENE_HEAD, 0, planar);
	CHK(f[16] == 0xcd && f[17] == 0xea);
	CHK(f[18] == 0x01 && f[19] == 0x01);              /* op-0101              */
	CHK(f[20] == 0x00 && f[21] == 0x18);              /* 24-byte payload      */
	CHK(f[23] == 0x22 && f[24] == 0xc8);              /* declares the total   */
	CHK(memcmp(f + 25, m.scene, REAC_SCENE_HEAD_BYTES) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(reac_ctrl_build_scene_step(m.scene_blk, m.scene, sizeof m.scene,
	                               REAC_SCENE_STEPS - 1) == 0);
	build_and_stamp(&m, f, REAC_M_EMIT_SCENE_TAIL, 0, planar);
	CHK(f[18] == 0x01 && f[19] == 0x02);              /* op-0102              */
	CHK(f[20] == 0x00 && f[21] == 0x0e);              /* 14-byte payload      */
	CHK(memcmp(f + 23, m.scene + REAC_SCENE_BYTES - REAC_SCENE_TAIL_BYTES,
	           REAC_SCENE_TAIL_BYTES) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* 5. chanmap window 0 (the fe frame) lists the marker + channels 0x00..0x06. */
	{
		const uint8_t *blk = M300_CHANMAP_FE + 2;   /* the 32-byte block */
		CHK(blk[5] == 0xfe);                     /* slot 0 = section marker */
		for (int c = 0; c <= 6; c++) {
			const uint8_t *t = blk + 5 + (c + 1) * 3;
			CHK(t[0] == (uint8_t)c && t[1] == 0x28 && t[2] == 0x00);
		}
	}

	/* 5b. the full sweep tiles the whole fabric: every slot 0x00..0x2f appears as a
	 * channel id in at least one window (#130 — else a box owning it stays mute). */
	{
		int seen[0x30] = { 0 };
		for (int i = 0; i < m.chanmap_nframes; i++) {
			build_and_stamp(&m, f, REAC_M_EMIT_CHANMAP, i, planar);
			const uint8_t *blk = f + 16 + 2;      /* the 32-byte block */
			for (int s = 0; s < 8; s++) {
				uint8_t ch = blk[5 + s * 3];
				if (ch != 0xfe && ch < 0x30)
					seen[ch] = 1;
			}
		}
		for (int ch = 0x00; ch <= 0x2f; ch++)
			CHK(seen[ch] == 1);                   /* whole fabric advertised */
	}

	/* control stamping is non-destructive: a FILLER frame's audio round-trips. */
	const struct reac_mode mode = { 48000, 40, 12 };
	build_and_stamp(&m, f, REAC_M_EMIT_FILLER, 0, planar);
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(f, REAC_FRAME_BYTES, &mode, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(f[16] == 0x00 && f[17] == 0x00);        /* FILLER keeps type 00 00 */

	/* 6. #130 fix 2: the FILLER control block [18:50] is the non-zero descriptor
	 * a real master stamps there (reac-captures/m{200,300}-s1608-establish-
	 * 2026-07-10.pcap: >1.1M captured FILLER frames from two consoles, 100%
	 * uniform-16x "00 xx" pairs, essentially never all-zero) — NOT the all-zero
	 * block our code used to leave. The exact live value is not offline-
	 * derivable (see reac_master.c's stamp_filler_descriptor comment); we only
	 * assert the STRUCTURE + that it is unambiguously non-zero, plus that the
	 * stamp is non-destructive (end marker + audio survive, as already checked
	 * above). */
	{
		int all_zero = 1;
		for (int i = 18; i < 50; i += 2) {
			CHK(f[i] == 0x00);              /* high byte constant, per the captures */
			CHK(f[i + 1] == m.filler_desc);   /* tracks the current probe */
			if (f[i] || f[i + 1])
				all_zero = 0;
		}
		CHK(!all_zero);                     /* never all-zero, unlike the old code */
	}
	CHK(f[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 &&
	    f[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);   /* end marker survives the stamp */

	/* ---- (a) NO-RX SOAK: the direct anti-#130 regression test ------------ */
	reac_master_init(&m, SRC, &idle, FPS);
	CHK(m.state == REAC_M_IDLE);
	uint16_t cnt = 0;
	int idx;
	long n_probe = 0, n_sub01 = 0, n_sub02 = 0, n_ann = 0, n_grant = 0, n_cm = 0;
	int cm_seen[49] = { 0 };                    /* which sweep windows were emitted */
	/* One chanmap window per control cycle (fps*10778/4000 slots ≈ 2.69 s), so
	 * the 49-window sweep needs 49 cycles ≈ 132 s — soak 140 s (~52 cycles) to
	 * cover the whole fabric. Burst rhythm asserted slot-exact: within a burst
	 * consecutive probes are probe_stride apart; across the pause the gap is
	 * cycle_len - burst_end. */
	long prev_probe = -1;
	for (long i = 0; i < 140L * FPS; i++) {     /* 140 s of slots, zero RX */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		CHK((int)e >= 0);
		switch (e) {
		case REAC_M_EMIT_ENROLL: CHK(0); break;   /* GRANTING-only; never in PROBING */
		case REAC_M_EMIT_SCENE_CHUNK:
			n_probe++;
			if (prev_probe >= 0) {
				long d = i - prev_probe;
				CHK(d == m.probe_stride ||               /* in-burst rhythm */
				    d == m.cycle_len - m.burst_end);     /* the probe-free pause */
			}
			prev_probe = i;
			break;
		case REAC_M_EMIT_SCENE_HEAD: n_sub01++; break;
		case REAC_M_EMIT_SCENE_TAIL: n_sub02++; break;
		case REAC_M_EMIT_ANNOUNCE: n_ann++;   break;
		case REAC_M_EMIT_CHANMAP:  n_cm++; CHK(idx >= 0 && idx < 49); cm_seen[idx] = 1; break;
		case REAC_M_EMIT_GRANT:    n_grant++; break;
		case REAC_M_EMIT_FILLER:   break;
		}
	}
	for (int w = 0; w < 49; w++)                 /* the cursor sweeps ALL 49 windows */
		CHK(cm_seen[w] == 1);
	CHK(m.state == REAC_M_PROBING);             /* NEVER advanced on a timer */
	CHK(n_grant == 0);                          /* invariant: NO grant without a validated JOIN */
	CHK(n_cm > 0);                              /* §4: chanmap advertised while unlinked */
	CHK(n_probe >= 51L * 341 && n_probe <= 53L * 341);   /* 341 chunks per transfer */
	CHK(n_sub01 >= 50 && n_sub01 <= 53);        /* sub01: once per cycle */
	CHK(n_sub02 >= 50 && n_sub02 <= 53);        /* sub02: once per cycle */
	CHK(n_cm    >= 50 && n_cm    <= 53);        /* chanmap: ONE window per cycle */
	CHK(n_ann   >= 138 && n_ann  <= 141);       /* cfea free-runs at ~1/s */

	/* ---- (a2) transfer choreography: the "specials" are the VALIDATED tags -----
	 * What an earlier RE transcribed as one-off "inventory specials" at in-burst
	 * indices 31..33 are chunks 31..33 of the body: our MAC at +0x340, "SYSP" at
	 * +0x368 and "SCEN" at +0x37c. Two of those three are exactly what the box's
	 * state-4 commit validates before promoting any head-amp, so this asserts they
	 * reach the WIRE on the right chunks — the body holding them is not enough
	 * when the failure mode is a middle chunk being overwritten. Every chunk also
	 * publishes its checksum as the FILLER descriptor. */
	reac_master_init(&m, SRC, &idle, FPS);
	cnt = 0;
	int specials_seen = 0;
	for (long i = 0; i < 2L * m.cycle_len; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e != REAC_M_EMIT_SCENE_CHUNK)
			continue;
		build_and_stamp(&m, f, e, idx, planar);
		CHK(reac_ctrl_checksum_verify(f) == 0);
		CHK(m.filler_desc == f[49]);              /* descriptor tracks EVERY chunk */
		int chunk = m.scene_step - 1;             /* 0-based index into the body */
		if (chunk == 31) {                        /* carries a MAC: OURS */
			CHK(memcmp(f + 25, SRC, 6) == 0);     /* block[7:13] = frame [25:31] */
			specials_seen++;
		} else if (chunk == 32) {                 /* the "SYSP" token */
			CHK(f[39] == 'S' && f[40] == 'Y' && f[41] == 'S' && f[42] == 'P');
			specials_seen++;
		} else if (chunk == 33) {                 /* the "SCEN" token */
			CHK(f[33] == 'S' && f[34] == 'C' && f[35] == 'E' && f[36] == 'N');
			specials_seen++;
		}
	}
	CHK(specials_seen == 2 * 3);                  /* MAC + SYSP + SCEN, EVERY transfer */

	/* ---- (b) the golden response sequence -------------------------------- */
	/* presence-flood alone must NOT grant (the golden rule) */
	for (int i = 0; i < 2000; i++) {
		CHK(reac_master_rx(&m, REAC_M_RX_BOX_BCAST_FILLER, BOX, NULL) == 0);
		slot(&m, NULL, &cnt);
	}
	CHK(m.state == REAC_M_PROBING && m.box_seen == 1);

	/* JOIN -> GRANTING on that exact event */
	deliver_scene(&m, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(memcmp(m.box_mac, BOX, 6) == 0);
	CHK(m.grant_attempts == 1);
	/* A cold-connect JOIN carries NO WIDTH, so there is still nothing to enroll:
	 * the master holds the ungranted window instead of granting a guess. The box
	 * declares itself a moment later (a real one's config-announce lands ~1 ms into
	 * GRANTING, see reac_pacer.c) and THAT is what fills the enrollment in. */
	CHK(reac_master_has_box(&m) == 0);
	CHK(m.grant_burst_len == 0);
	reac_master_set_box(&m, 16, 8, S1608_BASE);
	CHK(m.grant_burst_len == 56);

	/* collect the grant burst: ENROLL (0103000d) at slot 0, then the ~1.6 s
	 * grant_dwell hold (matching the measured M-200 ENROLL->grant gap: Δ1.503 s
	 * on matrix-m200-s0808-2026-07-11.pcap, Δ1.717 s on matrix-m200-s1608-
	 * 2026-07-11.pcap — the RX-driven rewrite dropped this dwell and started the
	 * burst the very next tick), THEN the 32 DISTINCT M-200 sweep blocks in
	 * order, byte-exact, 1-per-STRIDE. After the full enroll + dwell + burst the
	 * master SELF-COMPLETES to ESTABLISHED and HOLDS — the box goes quiet after
	 * the grant, so a master that waited for a post-burst unicast (or timed back
	 * to PROBING) made the box re-attempt forever (rig 2026-07-12: LED blinking
	 * faster, never solid). A real M-200 commits after granting and holds. */
	int grants = 0, last_grant_slot = -1, saw_enroll = 0;
	int span = m.grant_dwell + m.grant_burst_len * REAC_M_GRANT_STRIDE + 4;
	for (int i = 0; i < span; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e == REAC_M_EMIT_ENROLL) { CHK(i == 0); saw_enroll = 1; }
		CHK(e != REAC_M_EMIT_GRANT || i > m.grant_dwell);  /* no grant before the dwell elapses */
		if (e == REAC_M_EMIT_GRANT) {
			if (last_grant_slot >= 0)
				CHK(i - last_grant_slot == REAC_M_GRANT_STRIDE);  /* density */
			else
				CHK(i == m.grant_dwell + 1);        /* burst starts right after the dwell */
			last_grant_slot = i;
			CHK(idx == grants);                          /* blocks emitted in order */
			build_and_stamp(&m, f, e, idx, planar);
			CHK(memcmp(f + 16, m.grant_burst[idx], 34) == 0);  /* burst block, byte-exact */
			CHK(reac_ctrl_checksum_verify(f) == 0);
			grants++;
		}
	}
	CHK(saw_enroll);                            /* the pre-grant arm frame, once */
	CHK(grants == m.grant_burst_len);           /* all 32 blocks, byte-exact */
	CHK(m.state == REAC_M_ESTABLISHED);         /* self-completed + holds after the burst */

	/* 12 s established: the LOCKED cadence (measured slot-exact on matrix-m200-s0808,
	 * box SOLID) — ONLY cfea + chanmap, each metronomic at exactly 1/s, and NOTHING
	 * else (0 probe, 0 sub01/sub02). The chanmap at 1/s (not the 1/cycle hunt rate)
	 * is the box's sync keep-alive; under-sending it left the box BLINKING (#130). */
	long e_cm = 0, e_ann = 0, e_s1 = 0, e_s2 = 0, e_pr = 0;
	long cm_slot = -1, ann_slot = -1;
	for (long i = 0; i < 12L * FPS; i++) {
		reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL);   /* upstream flood */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		switch (e) {
		case REAC_M_EMIT_CHANMAP:
			e_cm++;
			CHK(idx >= 0 && idx < 49);              /* sweeps the fabric, cursor 0..48 */
			if (cm_slot >= 0)
				CHK(i - cm_slot == FPS);            /* metronomic 1/s (the lock heartbeat) */
			cm_slot = i;
			break;
		case REAC_M_EMIT_ANNOUNCE:
			e_ann++;
			if (ann_slot >= 0)
				CHK(i - ann_slot == FPS);           /* cfea at exactly 1/s */
			ann_slot = i;
			break;
		case REAC_M_EMIT_SCENE_HEAD:  e_s1++; break;
		case REAC_M_EMIT_SCENE_TAIL:  e_s2++; break;
		case REAC_M_EMIT_SCENE_CHUNK: e_pr++; break;
		case REAC_M_EMIT_FILLER:   break;
		default: CHK(0);                            /* no grant while established */
		}
	}
	CHK(m.state == REAC_M_ESTABLISHED);
	/* 12 s: chanmap ~12x + cfea ~12x, and ZERO probe/sub01/sub02 (the locked desk
	 * emits only the two 1/s keep-alives). */
	CHK(e_cm >= 11 && e_cm <= 13 && e_ann >= 11 && e_ann <= 13);
	CHK(e_s1 == 0 && e_s2 == 0);
	CHK(e_pr == 0);                                 /* ESTABLISHED emits ZERO probes (#130) */
	CHK(cm_slot != ann_slot);                                      /* phase-separated */

	/* ---- (c) GRANTING self-completes; ESTABLISHED holds; peer-gone is the only
	 *          forward-safety fallback (rig 2026-07-12) --------------------- */

	/* JOIN -> deliver ENROLL + the full 32-frame burst -> COMMIT to ESTABLISHED.
	 * The box goes quiet after the grant (it is settling its own TX_MUTE dwell), so
	 * a master that timed back to PROBING here made the box re-attempt forever (LED
	 * blinking faster, never solid). A real M-200 commits after granting and holds;
	 * the peer-gone budget below is the backward safety if the box truly vanishes. */
	reac_master_init(&m, SRC, &idle, FPS);
	cnt = 0;
	slot(&m, NULL, &cnt);                        /* IDLE -> PROBING on first slot */
	deliver_scene(&m, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	reac_master_set_box(&m, 16, 8, S1608_BASE);              /* the box declares itself */
	for (int i = 0; i < m.grant_dwell + m.grant_burst_len * REAC_M_GRANT_STRIDE + 1; i++)
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_ESTABLISHED);          /* self-completed after dwell + full burst */
	CHK(m.grant_attempts == 1);

	/* established then link_check_reload silent slots -> peer-gone, at exactly the
	 * budget's last frame. The budget is now the measured ~6.5 s M-200i hold
	 * (fps-scaled), NOT the old 600-frame constant (#130). */
	establish(&m, &cnt, BOX);                       /* JOIN + full burst + unicast accept */
	CHK(m.state == REAC_M_ESTABLISHED);
	CHK(m.link_check_reload == (FPS * 65) / 10);   /* ~6.5 s of frames */
	for (int i = 0; i < m.link_check_reload - 1; i++)
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_ESTABLISHED);          /* budget-1 silent slots: still held */
	slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_PROBING);              /* the last frame drains the budget */
	CHK(m.drop_reason == REAC_M_DROP_PEER_GONE);

	/* established + explicit BYE (hb selector 0x00) -> PROBING immediately */
	establish(&m, &cnt, BOX);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_BYE, BOX, NULL) == 1);
	CHK(m.state == REAC_M_PROBING && m.drop_reason == REAC_M_DROP_BYE);

	/* JOIN from a second MAC while established -> mac-change, re-grant the new */
	establish(&m, &cnt, BOX);
	deliver_scene(&m, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.drop_reason == REAC_M_DROP_MAC_CHANGE);
	CHK(memcmp(m.box_mac, BOX2, 6) == 0);        /* latched the new box */

	/* ---- ENROLL->grant DWELL (grant_dwell): a real M-200 waits ~1.6 s between
	 * ENROLL and the start of the grant burst (measured Δ1.503 s on
	 * matrix-m200-s0808-2026-07-11.pcap, Δ1.717 s on matrix-m200-s1608-2026-07-11
	 * .pcap). The box keeps cold-connecting on its own ~100 ms retry grid for the
	 * whole dwell, so a JOIN from the SAME box mid-dwell must NOT reset it —
	 * that was exactly the class of bug the repo's history calls out ("GRANTING
	 * ->ESTABLISHED in 0.25 ms... box LED blinking faster, never stabilising"),
	 * here in the other direction: a reset-happy dwell would never complete
	 * against a retrying box. A JOIN from a genuinely DIFFERENT box still
	 * re-latches (new box, fresh window). BOX2 is already latched from the
	 * mac-change re-grant above; m.grant_ticks == 0 fresh into this window. */
	CHK(m.grant_ticks == 0);
	CHK(m.grant_dwell > 10 * REAC_M_GRANT_STRIDE);   /* the dwell dwarfs one retry grid */
	for (int i = 0; i < 10 * REAC_M_GRANT_STRIDE; i++)   /* well inside the dwell */
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.grant_ticks == 10 * REAC_M_GRANT_STRIDE);      /* mid-dwell, no grant emitted yet */
	/* same-box retry mid-dwell: held, NOT reset */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 0);
	CHK(m.grant_ticks == 10 * REAC_M_GRANT_STRIDE && m.state == REAC_M_GRANTING);
	/* the burst must not have started before the dwell count, and must start
	 * right after it (checked exhaustively in the byte-oracle section above);
	 * consume the rest of the dwell here and confirm the very next tick is the
	 * first grant block. */
	while (m.grant_ticks <= m.grant_dwell)
		CHK(slot(&m, NULL, &cnt) != REAC_M_EMIT_GRANT);
	CHK(slot(&m, &idx, &cnt) == REAC_M_EMIT_GRANT && idx == 0);  /* right after the dwell */

	/* a DIFFERENT box's JOIN mid-dwell still restarts the window (new box). */
	static const uint8_t BOX3[6] = { 0x00, 0x40, 0xab, 0x03, 0x03, 0x03 };
	deliver_scene(&m, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX3, ZONEA_JOIN) == 1);
	CHK(m.grant_ticks == 0 && m.state == REAC_M_GRANTING);
	CHK(memcmp(m.box_mac, BOX3, 6) == 0);

	printf("OK: master byte oracle (generated chanmap/cfea-with-our-MAC, fixed "
	       "probe/sub01/sub02, grant-echo) + event-driven establishment (no timer "
	       "forward path; presence never grants; JOIN->GRANT-echo->first-unicast->"
	       "ESTABLISHED; backward-only fallbacks; continuous 5-message cadence both "
	       "states; free-running counter)\n");

	/* ---- task #156: 96 kHz emit is PARAMETERIZED off the 48k path -----------
	 * "96k is not anything different, same state diagram, doubled frequency" —
	 * verify the master rate follows --rate, NOT a compile-time 48k assumption,
	 * and that the frame SHAPE (REAC_FRAME_BYTES) is unaffected by either the
	 * profile or the rate (the #156 trailer RE: reac_tx.h / tests/test_reac_tx.c). */
	{
		const struct reac_mixer_profile *m200 = reac_mixer_profile_by_name("m200");
		const struct reac_mixer_profile *m300 = reac_mixer_profile_by_name("m300");
		const struct reac_mixer_profile *m5000 = reac_mixer_profile_by_name("m5000");
		CHK(m200 && m300 && m5000);
		CHK(m200->console_field == 0 && m300->console_field == 0);
		CHK(m5000->console_field == 1);

		int clamped;

		/* THE REQUESTED RATE IS HONOURED, for every desk profile: a stagebox
		 * adapts to the rate it is driven at, whatever desk the master claims
		 * to be, so which profile is selected never clamps --rate (issue #73).
		 *
		 * `clamped` is retained in the signature and is always 0 — kept so a
		 * real, demonstrated rule would have one place to live. */
		CHK(reac_mixer_resolve_rate(m200, 0, &clamped) == 48000 && !clamped);
		CHK(reac_mixer_resolve_rate(m200, 48000, &clamped) == 48000 && !clamped);
		CHK(reac_mixer_resolve_rate(m200, 96000, &clamped) == 96000 && !clamped);
		CHK(reac_mixer_resolve_rate(m300, 96000, &clamped) == 96000 && !clamped);
		CHK(reac_mixer_resolve_rate(m5000, 0, &clamped) == 48000 && !clamped);
		CHK(reac_mixer_resolve_rate(m5000, 96000, &clamped) == 96000 && !clamped);
		CHK(reac_mixer_resolve_rate(m5000, 48000, &clamped) == 48000 && !clamped);

		/* ONLY THREE PACES ARE LEGAL: 44.1, 48 and 96 kHz (operator, 2026-08-21).
		 * A Roland desk offers exactly these and drives the segment at the one
		 * chosen. Anything else is not a slower REAC, it is not REAC — it would
		 * need RE-PACING between the rig clock and the wire, which reac-pw cannot
		 * do. Refuse it and SAY SO via `clamped`, rather than putting a cadence on
		 * the wire no box can follow and calling it configuration. */
		CHK(reac_mixer_resolve_rate(m200,  44100, &clamped) == 44100 && !clamped);
		CHK(reac_mixer_resolve_rate(m5000, 44100, &clamped) == 44100 && !clamped);
		CHK(reac_mixer_resolve_rate(m200,  88200, &clamped) == 48000 && clamped);
		CHK(reac_mixer_resolve_rate(m5000, 32000, &clamped) == 48000 && clamped);
		CHK(reac_mixer_resolve_rate(m300,  1,     &clamped) == 48000 && clamped);

		/* fps = rate/REAC_SAMPLES_PER_PKT (reac_sink_node.c) for each resolved
		 * rate; the pacer's per-fps period (reac_pacer_period_ns) is already
		 * covered by test_reac_pacer.c — pin the rate->fps mapping here. */
		int fps_m200_48k  = reac_mixer_resolve_rate(m200, 0, NULL) / REAC_SAMPLES_PER_PKT;
		int fps_m5000_96k = reac_mixer_resolve_rate(m5000, 96000, NULL) / REAC_SAMPLES_PER_PKT;
		CHK(fps_m200_48k == 4000);
		CHK(fps_m5000_96k == 8000);

		/* frame SHAPE is identical for both: reac_downstream_build takes no mixer/rate
		 * input at all, so the emitted frame is REAC_FRAME_BYTES regardless. */
		static const uint8_t src[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
		uint8_t frame[REAC_FRAME_BYTES];
		float *planar[REAC_MAX_CHANNELS] = { 0 };
		CHK(reac_downstream_build(frame, planar, 0, REAC_SAMPLES_PER_PKT, 0, src) == REAC_FRAME_BYTES);

		/* the master FSM itself scales purely off fps for both profiles (no 48k
		 * assumption baked into reac_master_init: cycle_len/chanmap_off/etc are
		 * fps*K/4000, see reac_master_init). console_field only changes the
		 * stamped identity bytes, not the cadence math. */
		struct reac_master mm200, mm5000;
		/* any source MAC does — the cadence math under test is MAC-independent */
		reac_master_init(&mm200, src, NULL, fps_m200_48k);
		reac_master_init(&mm5000, src, NULL, fps_m5000_96k);
		CHK(mm200.fps == 4000 && mm5000.fps == 8000);
		CHK(mm5000.cycle_len == mm200.cycle_len * 2);      /* fps doubled -> cycle doubled */
		CHK(mm5000.chanmap_off == mm200.chanmap_off * 2);

		printf("OK: --mixer m5000 resolves 96 kHz (48k for other profiles "
		       "unchanged), fps = rate/12 (4000 @48k / 8000 @96k), frame size "
		       "stays REAC_FRAME_BYTES for both\n");
	}

	/* ================================================================== *
	 * THE BOX IS WHAT THE WIRE SAYS IT IS (2026-08-05)
	 *
	 * The master used to hold a 16-channel enrollment for head-amp slots
	 * 0x20..0x2f before it had seen anything — REAC_CONSOLE_CFG_S1608's
	 * in_channels, hard-coded in reac_sink_node.c. These cases pin the three
	 * places that fabrication could reach the wire, each stated as a VALUE on
	 * the emitted records rather than as a shape.
	 * ================================================================== */
	{
		struct reac_master mb;
		uint16_t bc = 0;

		/* (a) STARTUP. No box, no allocation, no sweep — and that is a normal
		 * running state, not a degraded one: reac-pw comes up before anything is
		 * plugged in and waits. */
		reac_master_init(&mb, SRC, &idle, FPS);
		CHK(reac_master_has_box(&mb) == 0);
		CHK(mb.alloc.width == 0 && mb.alloc.base == 0);
		CHK(mb.grant_burst_len == 0);
		/* and it cannot stamp a grant it does not have */
		CHK(reac_master_stamp(&mb, f, REAC_M_EMIT_GRANT, 0) == -1);

		/* (b) THE SEED-vs-WIRE CASE, which is the whole point. A cold-connect
		 * JOIN carries no width. Under the fabricated seed this master would now
		 * be holding a 16-wide base-0x20 sweep for a box that has said nothing at
		 * all, and would put it on the wire — enrolling slots 0x20..0x2f for a box
		 * whose inputs live at 0x00..0x07, which links, streams audio, and then
		 * ignores every head-amp record (reac_grant.h, live 2026-07-17). Drive the
		 * ENTIRE dwell and assert what actually reaches the wire: nothing. */
		slot(&mb, NULL, &bc);                       /* IDLE -> PROBING */
		deliver_scene(&mb, &bc);
		CHK(reac_master_rx(&mb, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
		CHK(mb.state == REAC_M_GRANTING);
		CHK(reac_master_has_box(&mb) == 0);         /* the JOIN said nothing */
		int emitted_grants = 0, emitted_enrolls = 0;
		for (int i = 0; i < mb.grant_dwell; i++) {
			enum reac_master_emit e = slot(&mb, NULL, &bc);
			if (e == REAC_M_EMIT_GRANT)  emitted_grants++;
			if (e == REAC_M_EMIT_ENROLL) emitted_enrolls++;
		}
		CHK(emitted_grants == 0);      /* NOTHING enrolled: we do not guess a box */
		CHK(emitted_enrolls == 1);     /* the wide-safe arm frame, once */
		CHK(mb.state == REAC_M_GRANTING);           /* still holding, not established */

		/* (c) THE BOX DECLARES ITSELF — an S-0808, 8 inputs. The window restarts
		 * and the burst that reaches the wire enrolls 0x00..0x07, every record. */
		reac_master_set_box(&mb, 8, 8, S0808_BASE);
		CHK(reac_master_has_box(&mb) == 1);
		CHK(mb.alloc.base == 0x00 && mb.alloc.width == 8);
		CHK(mb.grant_burst_len == 32);              /* 8 + 8*3 */
		CHK(mb.grant_ticks == 0);                   /* window restarted */
		int ga_records = 0;
		emitted_grants = 0;
		for (int i = 0; i < mb.grant_dwell + mb.grant_burst_len * REAC_M_GRANT_STRIDE + 2; i++) {
			int gi;
			enum reac_master_emit e = slot(&mb, &gi, &bc);
			if (e != REAC_M_EMIT_GRANT)
				continue;
			emitted_grants++;
			build_and_stamp(&mb, f, e, gi, planar);
			CHK(f[16] == 0xcd && f[17] == 0xea);    /* a real cdea grant frame */
			CHK(reac_ctrl_checksum_verify(f) == 0);
			if (f[32] == 0x12 && f[33] == 0x12 && f[34] == 0x01 && f[35] == 0x01) {
				ga_records++;
				CHK(f[36] <= 0x07);                 /* the S-0808's own slots */
			}
		}
		CHK(emitted_grants == 32);
		CHK(ga_records == 8 * 3);                   /* phantom+pad+sens per input */
		CHK(mb.state == REAC_M_ESTABLISHED);

		/* (d) A BOX SWAP RE-DERIVES EVERYTHING. The 8-wide box goes; the master
		 * forgets it, so nothing of the departed box's placement can be inherited
		 * by whatever joins next — which matters most for the box we CANNOT
		 * identify, since no recognition edge would ever fire to correct it. */
		CHK(reac_master_rx(&mb, REAC_M_RX_BOX_BYE, BOX, NULL) == 1);
		CHK(mb.state == REAC_M_PROBING);
		CHK(reac_master_has_box(&mb) == 0);
		CHK(mb.alloc.width == 0 && mb.grant_burst_len == 0);
		/* a DIFFERENT box joins and declares 16 inputs: base moves to 0x20 */
		deliver_scene(&mb, &bc);
		CHK(reac_master_rx(&mb, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 1);
		reac_master_set_box(&mb, 16, 8, S1608_BASE);
		CHK(mb.alloc.base == 0x20 && mb.alloc.width == 16);
		CHK(mb.grant_burst_len == 56);
		for (int i = 0; i < mb.grant_burst_len; i++) {
			const uint8_t *r = mb.grant_burst[i];
			if (r[16] == 0x12 && r[17] == 0x12 && r[18] == 0x01 && r[19] == 0x01)
				CHK(r[20] >= 0x20 && r[20] <= 0x2f);   /* NOT the old box's slots */
		}

		/* (e) A BOX THAT NEVER DECLARES ITSELF is not guessed at. The hold expires
		 * and we go back to probing — still inviting, never enrolled at a made-up
		 * base. Recoverable and loud beats plausible and wrong. */
		struct reac_master mu;
		uint16_t uc = 0;
		reac_master_init(&mu, SRC, &idle, FPS);
		slot(&mu, NULL, &uc);
		deliver_scene(&mu, &uc);
		CHK(reac_master_rx(&mu, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
		int held_grants = 0;
		for (int i = 0; i <= mu.grant_dwell; i++) {
			enum reac_master_emit e = slot(&mu, NULL, &uc);
			if (e == REAC_M_EMIT_GRANT)
				held_grants++;
		}
		CHK(held_grants == 0);
		CHK(mu.state == REAC_M_PROBING);
		CHK(mu.drop_reason == REAC_M_DROP_BOX_UNKNOWN);

		/* (f) AND IT CANNOT ESTABLISH UNENROLLED. The box's accept establishes
		 * only once the full sweep has been delivered; with no sweep at all that
		 * predicate must read FALSE, not vacuously true — otherwise the box links
		 * with audio flowing and not one head-amp slot claimed, which is this same
		 * failure reached from the opposite side. */
		struct reac_master mn;
		uint16_t nc = 0;
		reac_master_init(&mn, SRC, &idle, FPS);
		slot(&mn, NULL, &nc);
		deliver_scene(&mn, &nc);
		CHK(reac_master_rx(&mn, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
		CHK(reac_master_rx(&mn, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 0);
		CHK(mn.state == REAC_M_GRANTING);           /* held, NOT established */
		CHK(reac_master_rx(&mn, REAC_M_RX_BOX_HEARTBEAT, BOX, NULL) == 0);
		CHK(mn.state == REAC_M_GRANTING);
		/* and the predicate itself, at the boundary the emit loop cannot reach:
		 * "the full sweep has left the wire" must read FALSE for an EMPTY sweep,
		 * not trivially true because zero blocks take zero slots to send. The hold
		 * above happens to drop out of GRANTING first today, so this is the guard
		 * that keeps the predicate honest if that bound ever changes. */
		mn.grant_ticks = mn.grant_dwell + 1;    /* past the delivery threshold */
		CHK(mn.grant_burst_len == 0);
		CHK(reac_master_rx(&mn, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 0);
		CHK(mn.state == REAC_M_GRANTING);       /* still NOT established */

		printf("OK: the box comes from the wire — a master with no declaration has "
		       "no allocation and no sweep, a cold-connect JOIN alone enrolls "
		       "NOTHING, the declared width places the sweep (S-0808 -> 0x00..0x07, "
		       "S-1608 -> 0x20..0x2f) on every emitted group-A record, a swap "
		       "re-derives from scratch, and an undeclared box is neither guessed "
		       "at nor established\n");
	}

	return 0;
}
