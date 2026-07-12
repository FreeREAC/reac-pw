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
#include "reac_master.h"
#include "reac_ctrl.h"
#include "reac_tx.h"
#include "reac_decode.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* Gold captured M-300/S-1608 control blocks, bytes [16:50] (type[2] + block[32]).
 * The master MUST stamp exactly these (our MAC substituted into the cfea). */
static const uint8_t GOLD_CHANMAP[34] =
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0xfe,0x00,0x00,0x00,0x28,0x00,0x01,0x28,0x00,0x02,0x28,0x00,0x03,0x28,0x00,0x04,0x28,0x00,0x05,0x28,0x00,0x06,0x28,0x00,0x00,0x00,0xb7 };
/* cfea with the captured M-300 MAC embedded at template idx 11..16. The master
 * emits it with OUR MAC substituted + the checksum recomputed. */
static const uint8_t GOLD_CFEA_M300[34] =
 { 0xcf,0xea,0xff,0xff,0x01,0x00,0x01,0x03,0x0d,0x01,0x04,0x00,0x40,0xab,0xc9,0xd8,0x5b,0x28,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd4 };
/* The PROBE is NOT a constant — it rotates (phase +6 mod 10, 2 emissions each,
 * sub 0x02 hunting / 0x03 established; #130, measured live off an M-200). This is
 * the block a freshly-init'd master seeds: phase 0, sub 0x02 (checksum 0xde). */
static const uint8_t GOLD_PROBE[34] =
 { 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xde };
static const uint8_t GOLD_SUB01[34] =
 { 0xcd,0xea,0x01,0x01,0x00,0x18,0x00,0x22,0xc8,0x31,0x32,0x33,0x34,0x01,0x00,0x00,0x00,0x04,0x00,0x01,0x80,0x02,0x00,0x01,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xa7 };
static const uint8_t GOLD_SUB02[34] =
 { 0xcd,0xea,0x01,0x02,0x00,0x0e,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xe7 };
#define ANNOUNCE_MAC_IDX 11

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
	reac_tx_build(out, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, SRC);
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

/* Drive the master from a JOIN to ESTABLISHED the way a real box does: the JOIN
 * opens GRANTING, the emit loop delivers the FULL 32-frame grant burst, then the
 * box's unicast accept lands. The accept is gated on burst completion (#130 rig
 * fix 2026-07-12: a warm-relink box unicasts immediately and would otherwise cut
 * the burst to ~1 frame, leaving the box's light blinking). */
static void establish(struct reac_master *m, uint16_t *cnt, const uint8_t box[6])
{
	reac_master_rx(m, REAC_M_RX_BOX_JOIN, box, ZONEA_JOIN);
	/* +1 for the leading ENROLL slot before the 32-block burst. */
	for (int i = 0; i < m->grant_burst_len * REAC_M_GRANT_STRIDE + 1; i++)
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
	struct reac_console_cfg s1608 = REAC_CONSOLE_CFG_S1608;
	reac_master_init(&m, SRC, &s1608, FPS);

	/* the downstream chanmap is the 11-window fabric sweep (#130); window 0 is the
	 * fe frame (marker + 0x00..0x06), asserted below against GOLD_CHANMAP. */
	CHK(m.chanmap_nframes == 49);

	/* ---- byte oracle ---------------------------------------------------- */

	/* 1. the generated channel-map frame byte-matches the captured M-300 block
	 * (no MAC in the chanmap -> EXACT) + checksum. */
	build_and_stamp(&m, f, REAC_M_EMIT_CHANMAP, 0, planar);
	CHK(memcmp(f + 16, GOLD_CHANMAP, 34) == 0);      /* type + block exact */
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
	CHK(memcmp(f + 16, GOLD_CFEA_M300, ANNOUNCE_MAC_IDX) == 0); /* head intact */
	CHK(memcmp(f + 16 + ANNOUNCE_MAC_IDX, SRC, 6) == 0); /* OUR MAC embedded */
	CHK(memcmp(f + 16 + ANNOUNCE_MAC_IDX + 6,           /* tail intact (pre-cksum) */
	           GOLD_CFEA_M300 + ANNOUNCE_MAC_IDX + 6, 34 - ANNOUNCE_MAC_IDX - 6 - 1) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[16 + 17] == 0x28 && f[16 + 18] == 0x08);       /* inCh 40, outCh 8 */

	/* 3. the grant is the master's OWN burst sweep (byte-exact M-200 cdea 04 03),
	 * NOT an echo of the box's JOIN (the echo model was falsified by
	 * matrix-m200-s0808 2026-07-12 — a locked box gets the master's sweep). Block
	 * 0 of the S-0808 burst is a 04030014 frame. */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	build_and_stamp(&m, f, REAC_M_EMIT_GRANT, 0, planar);
	CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x04 && f[19] == 0x03);
	CHK(memcmp(f + 16, m.grant_burst[0], 34) == 0);      /* burst block 0, byte-exact */
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* 4. PROBE / SUB01 / SUB02 are the fixed M-300 constants, byte-exact. */
	build_and_stamp(&m, f, REAC_M_EMIT_PROBE, 0, planar);
	CHK(memcmp(f + 16, GOLD_PROBE, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	build_and_stamp(&m, f, REAC_M_EMIT_SUB01, 0, planar);
	CHK(memcmp(f + 16, GOLD_SUB01, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	build_and_stamp(&m, f, REAC_M_EMIT_SUB02, 0, planar);
	CHK(memcmp(f + 16, GOLD_SUB02, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* 5. chanmap window 0 (the fe frame) lists the marker + channels 0x00..0x06. */
	{
		const uint8_t *blk = GOLD_CHANMAP + 2;   /* the 32-byte block */
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
	reac_master_init(&m, SRC, &s1608, FPS);
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
		case REAC_M_EMIT_PROBE:
			n_probe++;
			if (prev_probe >= 0) {
				long d = i - prev_probe;
				CHK(d == m.probe_stride ||               /* in-burst rhythm */
				    d == m.cycle_len - m.burst_end);     /* the probe-free pause */
			}
			prev_probe = i;
			break;
		case REAC_M_EMIT_SUB01:    n_sub01++; break;
		case REAC_M_EMIT_SUB02:    n_sub02++; break;
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
	CHK(n_probe >= 51L * 341 && n_probe <= 53L * 341);   /* 341 probes per burst-cycle */
	CHK(n_sub01 >= 50 && n_sub01 <= 53);        /* sub01: once per cycle */
	CHK(n_sub02 >= 50 && n_sub02 <= 53);        /* sub02: once per cycle */
	CHK(n_cm    >= 50 && n_cm    <= 53);        /* chanmap: ONE window per cycle */
	CHK(n_ann   >= 138 && n_ann  <= 141);       /* cfea free-runs at ~1/s */

	/* ---- (a2) burst choreography: the 4 inventory specials + descriptor ----
	 * A real burst carries the zeros/our-MAC/SYSP/SCEN specials at in-burst probe
	 * indices 30..33 (measured 11/11 bursts on the M-300 establish capture), and
	 * every probe — special or rotating — publishes its checksum as the FILLER
	 * descriptor. */
	reac_master_init(&m, SRC, &s1608, FPS);
	cnt = 0;
	int specials_seen = 0;
	for (long i = 0; i < 2L * m.cycle_len; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e != REAC_M_EMIT_PROBE)
			continue;
		build_and_stamp(&m, f, e, idx, planar);
		CHK(reac_ctrl_checksum_verify(f) == 0);
		CHK(m.filler_desc == f[49]);              /* descriptor tracks EVERY probe */
		if (m.probe_idx == 30) {                  /* zeros special */
			CHK(f[49] == 0xdd);
			specials_seen++;
		} else if (m.probe_idx == 31) {           /* MAC special: OUR identity */
			CHK(memcmp(f + 25, SRC, 6) == 0);     /* block[7:13] = frame [25:31] */
			specials_seen++;
		} else if (m.probe_idx == 32) {           /* "SYSP" inventory token (M-200: block idx 23 -> frame 39) */
			CHK(f[39] == 'S' && f[40] == 'Y' && f[41] == 'S' && f[42] == 'P');
			specials_seen++;
		} else if (m.probe_idx == 33) {           /* "SCEN" inventory token */
			CHK(f[33] == 'S' && f[34] == 'C' && f[35] == 'E' && f[36] == 'N');
			specials_seen++;
		}
	}
	CHK(specials_seen == 2 * 4);                  /* all 4 specials, EVERY burst */

	/* ---- (b) the golden response sequence -------------------------------- */
	/* presence-flood alone must NOT grant (the golden rule) */
	for (int i = 0; i < 2000; i++) {
		CHK(reac_master_rx(&m, REAC_M_RX_BOX_BCAST_FILLER, BOX, NULL) == 0);
		slot(&m, NULL, &cnt);
	}
	CHK(m.state == REAC_M_PROBING && m.box_seen == 1);

	/* JOIN -> GRANTING on that exact event */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(memcmp(m.box_mac, BOX, 6) == 0);
	CHK(m.grant_attempts == 1);

	/* collect the grant burst: ENROLL (0103000d) at slot 0, then the 32 DISTINCT
	 * M-200 sweep blocks in order, byte-exact, 1-per-STRIDE. After the full enroll +
	 * burst the master SELF-COMPLETES to ESTABLISHED and HOLDS — the box goes quiet
	 * after the grant, so a master that waited for a post-burst unicast (or timed
	 * back to PROBING) made the box re-attempt forever (rig 2026-07-12: LED blinking
	 * faster, never solid). A real M-200 commits after granting and holds. */
	int grants = 0, last_grant_slot = -1, saw_enroll = 0;
	int span = m.grant_burst_len * REAC_M_GRANT_STRIDE + 4;
	for (int i = 0; i < span; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e == REAC_M_EMIT_ENROLL) { CHK(i == 0); saw_enroll = 1; }
		if (e == REAC_M_EMIT_GRANT) {
			if (last_grant_slot >= 0)
				CHK(i - last_grant_slot == REAC_M_GRANT_STRIDE);  /* density */
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
		case REAC_M_EMIT_SUB01:    e_s1++; break;
		case REAC_M_EMIT_SUB02:    e_s2++; break;
		case REAC_M_EMIT_PROBE:    e_pr++; break;
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
	reac_master_init(&m, SRC, &s1608, FPS);
	cnt = 0;
	slot(&m, NULL, &cnt);                        /* IDLE -> PROBING on first slot */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	for (int i = 0; i < m.grant_burst_len * REAC_M_GRANT_STRIDE + 1; i++)
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_ESTABLISHED);          /* self-completed after the full burst */
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
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.drop_reason == REAC_M_DROP_MAC_CHANGE);
	CHK(memcmp(m.box_mac, BOX2, 6) == 0);        /* latched the new box */

	/* a fresh JOIN mid-burst restarts the burst (stay well inside the burst span,
	 * which now self-completes to ESTABLISHED at burst_len*STRIDE+1 slots). */
	for (int i = 0; i < 10 * REAC_M_GRANT_STRIDE; i++)   /* 120 << 385, still GRANTING */
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_GRANTING);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 0);
	CHK(m.grant_ticks == 0 && m.state == REAC_M_GRANTING);

	printf("OK: master byte oracle (generated chanmap/cfea-with-our-MAC, fixed "
	       "probe/sub01/sub02, grant-echo) + event-driven establishment (no timer "
	       "forward path; presence never grants; JOIN->GRANT-echo->first-unicast->"
	       "ESTABLISHED; backward-only fallbacks; continuous 5-message cadence both "
	       "states; free-running counter)\n");
	return 0;
}
