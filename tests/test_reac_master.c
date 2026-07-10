// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* MASTER-role handshake: byte oracle + the EVENT-DRIVEN establishment (#130).
 *
 * Byte oracle (against the gold-capture bytes, reac-captures/wired-reac-a-
 * bothdirs, master 00:40:ab:ca:15:4d, + the §6/§13d transcription):
 *   1. the established channel-map frames byte-match the captured M-5000 blocks
 *      AND satisfy Sum(block[18..49]) mod 256 == 0;
 *   2. the cfea announce is the captured template with OUR src MAC embedded
 *      (identity fix: on-wire identity must match the L2 source) + checksum;
 *   3. the grant ECHOES the received JOIN block verbatim;
 *   4. the probe cycles the 00-dominant sub-state table with valid checksums;
 *   5. the chanmap cursor walks the 40-ch map with no duplicates.
 *
 * FSM (the anti-#130 core): NO timer ever advances toward ESTABLISHED —
 *   a. no-RX soak: 60 s of slots with zero RX events stays PROBING, zero grants;
 *   b. the golden M-5000 response sequence: presence never grants; JOIN ->
 *      GRANTING (echo burst at 1-per-12 density); first box unicast ->
 *      ESTABLISHED; dual independent 1/s chanmap + cfea streams;
 *   c. safety fallbacks only move BACKWARD: grant-window expiry, 600-frame
 *      peer-gone budget, explicit BYE, box-MAC change;
 *   d. announce identity embeds OUR MAC;
 *   e. the counter free-runs monotonically (mod 2^16) across every transition. */
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

/* Gold captured control blocks, bytes [16:50] (type[2] + block[32]). These are
 * the independent oracle: the master MUST stamp exactly these (counter aside). */
static const uint8_t GOLD_CHANMAP[REAC_M_CHANMAP_FRAMES][34] = {
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x14,0x28,0x00,0x15,0x28,0x00,0x16,0x28,0x00,0x17,0x28,0x00,0x18,0x28,0x00,0x19,0x28,0x00,0x1a,0x28,0x00,0x1b,0x28,0x00,0x00,0x00,0xe6 },
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x1c,0x28,0x00,0x1d,0x28,0x00,0x1e,0x28,0x00,0x1f,0x28,0x00,0x20,0x28,0x00,0x21,0x28,0x00,0x22,0x28,0x00,0x23,0x28,0x00,0x00,0x00,0xa6 },
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x24,0x28,0x00,0x25,0x28,0x00,0x26,0x28,0x00,0x27,0x28,0x00,0x28,0x38,0x00,0x29,0x38,0x00,0x2a,0x38,0x00,0x2b,0x38,0x00,0x00,0x00,0x26 },
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x2c,0x38,0x00,0x2d,0x38,0x00,0x2e,0x38,0x00,0x2f,0x38,0x00,0xfe,0x01,0x00,0x00,0x28,0x00,0x01,0x28,0x00,0x02,0x28,0x00,0x00,0x00,0xd2 },
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x03,0x28,0x00,0x04,0x28,0x00,0x05,0x28,0x00,0x06,0x28,0x00,0x07,0x28,0x00,0x08,0x28,0x00,0x09,0x28,0x00,0x0a,0x28,0x00,0x00,0x00,0x6e },
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0x0b,0x28,0x00,0x0c,0x28,0x00,0x0d,0x28,0x00,0x0e,0x28,0x00,0x0f,0x28,0x00,0x10,0x28,0x00,0x11,0x28,0x00,0x12,0x28,0x00,0x00,0x00,0x2e },
};
/* The captured cfea announce TEMPLATE (embeds the captured desk's MAC at
 * template idx 11..16). The master must emit this with OUR MAC substituted
 * and the checksum recomputed — never the cloned ca:15:4d identity. */
static const uint8_t GOLD_ANNOUNCE_TMPL[34] =
 { 0xcf,0xea,0xff,0xff,0x01,0x00,0x01,0x03,0x0d,0x01,0x04,0x00,0x40,0xab,0xca,0x15,0x4d,0x28,0x10,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x9a };
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
	reac_master_init(&m, SRC, FPS);

	/* ---- byte oracle ---------------------------------------------------- */

	/* 1. each channel-map frame byte-matches the captured master block + checksum */
	for (int i = 0; i < REAC_M_CHANMAP_FRAMES; i++) {
		build_and_stamp(&m, f, REAC_M_EMIT_CHANMAP, i, planar);
		CHK(memcmp(f + 16, GOLD_CHANMAP[i], 34) == 0);   /* type + block exact */
		CHK(reac_ctrl_checksum_verify(f) == 0);          /* Sum[18..49]%256==0 */
		CHK(f[16] == 0xcd && f[17] == 0xea);             /* cdea */
		CHK(f[18] == 0x01 && f[19] == 0x03);             /* established sub-state */
		CHK(f[20] == 0x00 && f[21] == 0x19);             /* BE len 0x0019 */
		CHK(f[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 &&
		    f[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);
	}

	/* 2. cfea announce = captured template with OUR MAC + recomputed checksum
	 * (identity fix, finding 3 — NEVER the cloned desk MAC ca:15:4d). */
	build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0, planar);
	CHK(f[16] == 0xcf && f[17] == 0xea);                 /* cfea */
	CHK(memcmp(f + 16, GOLD_ANNOUNCE_TMPL, ANNOUNCE_MAC_IDX) == 0); /* head intact */
	CHK(memcmp(f + 16 + ANNOUNCE_MAC_IDX, SRC, 6) == 0); /* OUR MAC embedded */
	CHK(memcmp(f + 16 + ANNOUNCE_MAC_IDX + 6,           /* tail intact (pre-cksum) */
	           GOLD_ANNOUNCE_TMPL + ANNOUNCE_MAC_IDX + 6, 34 - ANNOUNCE_MAC_IDX - 6 - 1) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[33] == 0x28 && f[34] == 0x10);                 /* inCh 40, outCh 16 */

	/* 3. the grant is the ECHO of the received JOIN block, verbatim */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	build_and_stamp(&m, f, REAC_M_EMIT_GRANT, 0, planar);
	CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x04 && f[19] == 0x03);
	CHK(memcmp(f + 18, ZONEA_JOIN, 32) == 0);            /* byte-for-byte echo */
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* 4. the probe cycles the 00-dominant table, each with a valid checksum */
	for (int i = 0; i < 8; i++) {
		build_and_stamp(&m, f, REAC_M_EMIT_PROBE, i, planar);
		CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x01);   /* cdea 01 */
		CHK(f[19] == REAC_M_PROBE_CYCLE[i]);                    /* the sub-state */
		CHK(reac_ctrl_checksum_verify(f) == 0);                 /* re-applied cksum */
	}

	/* 5. the chanmap walk covers the channel space with NO duplicates (records
	 * are 3 bytes (ch#, flag, 00) from block byte [5]; one slot (0x13) falls in
	 * a 7th frame the 120 s capture missed, so coverage is >=39 of 40). */
	int seen[64] = { 0 }, covered = 0;
	for (int i = 0; i < REAC_M_CHANMAP_FRAMES; i++) {
		const uint8_t *blk = GOLD_CHANMAP[i] + 2;   /* the 32-byte block */
		for (int r = 5; r + 2 < 31; r += 3) {       /* records start at block[5] */
			uint8_t ch = blk[r], flag = blk[r + 1];
			if (flag == 0x28 || flag == 0x38) {
				CHK(ch < 64);
				seen[ch]++;
			}
		}
	}
	for (int ch = 0; ch < 48; ch++)
		CHK(seen[ch] <= 1);                 /* no channel listed twice */
	for (int ch = 0; ch < 40; ch++)
		covered += seen[ch];
	CHK(covered >= 39);                     /* near-complete 40-ch coverage */

	/* control stamping is non-destructive: a FILLER frame's audio round-trips. */
	const struct reac_mode mode = { 48000, 40, 12 };
	build_and_stamp(&m, f, REAC_M_EMIT_FILLER, 0, planar);
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(f, REAC_FRAME_BYTES, &mode, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(f[16] == 0x00 && f[17] == 0x00);        /* FILLER keeps type 00 00 */

	/* ---- (a) NO-RX SOAK: the direct anti-#130 regression test ------------ */
	reac_master_init(&m, SRC, FPS);
	CHK(m.state == REAC_M_IDLE);
	uint16_t cnt = 0;
	int idx;
	int n_probe = 0, n_ann = 0, n_grant = 0, n_cm = 0;
	int sub_hist[256] = { 0 };
	for (long i = 0; i < 60L * FPS; i++) {      /* 60 s of slots, zero RX */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		CHK((int)e >= 0);
		if (e == REAC_M_EMIT_PROBE) { n_probe++; sub_hist[REAC_M_PROBE_CYCLE[idx & 7]]++; }
		else if (e == REAC_M_EMIT_ANNOUNCE) n_ann++;
		else if (e == REAC_M_EMIT_GRANT) n_grant++;
		else if (e == REAC_M_EMIT_CHANMAP) n_cm++;
	}
	CHK(m.state == REAC_M_PROBING);             /* NEVER advanced on a timer */
	CHK(n_grant == 0 && n_cm == 0);             /* old code emitted 1200 grants at t=1s */
	CHK(n_probe >= 120 * 60 && n_probe <= 250 * 60);   /* 120-250 probes/s band */
	CHK(sub_hist[0x00] > sub_hist[0x03] && sub_hist[0x00] > sub_hist[0x01] &&
	    sub_hist[0x00] > sub_hist[0x02]);        /* 00-dominant */
	CHK(sub_hist[0x03] > 0 && sub_hist[0x01] > 0 && sub_hist[0x02] > 0);
	CHK(n_ann >= 55 && n_ann <= 65);            /* cfea ~1/s even unlinked */

	/* ---- (b) the golden M-5000 response sequence ------------------------- */
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

	/* collect the grant burst: echo bytes, 1-per-12 density, ~100 over 150 ms */
	int grants = 0, last_grant_slot = -1;
	int mid = m.grant_frames / 2;
	for (int i = 0; i < mid; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e == REAC_M_EMIT_GRANT) {
			if (last_grant_slot >= 0)
				CHK(i - last_grant_slot == REAC_M_GRANT_STRIDE);  /* density */
			last_grant_slot = i;
			grants++;
			build_and_stamp(&m, f, e, idx, planar);
			CHK(memcmp(f + 18, ZONEA_JOIN, 32) == 0);   /* verbatim echo */
			CHK(reac_ctrl_checksum_verify(f) == 0);
		}
	}
	CHK(m.state == REAC_M_GRANTING);
	CHK(grants == mid / REAC_M_GRANT_STRIDE);   /* ~50 at mid-window (100/window) */

	/* first box unicast mid-window -> ESTABLISHED immediately */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 1);
	CHK(m.state == REAC_M_ESTABLISHED);

	/* 12 s established: TWO independent 1/s streams (not 0.5 Hz alternation);
	 * the cursor walks 0..5 cyclically. Box RX every slot keeps HOLD loaded. */
	int cm = 0, ann = 0, last_cm_idx = -1, walked = 0;
	long cm_slot = -1, ann_slot = -1;
	for (long i = 0; i < 12L * FPS; i++) {
		reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL);   /* upstream flood */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		if (e == REAC_M_EMIT_CHANMAP) {
			cm++;
			if (last_cm_idx >= 0)
				CHK(idx == (last_cm_idx + 1) % REAC_M_CHANMAP_FRAMES);
			last_cm_idx = idx;
			walked++;
			if (cm_slot >= 0)
				CHK(i - cm_slot == FPS);            /* exactly 1/s */
			cm_slot = i;
		} else if (e == REAC_M_EMIT_ANNOUNCE) {
			ann++;
			if (ann_slot >= 0)
				CHK(i - ann_slot == FPS);           /* exactly 1/s */
			ann_slot = i;
		} else {
			CHK(e == REAC_M_EMIT_FILLER);
		}
	}
	CHK(m.state == REAC_M_ESTABLISHED);
	CHK(cm >= 11 && cm <= 13 && ann >= 11 && ann <= 13);   /* both every second */
	CHK(walked >= 11);
	CHK(cm_slot != ann_slot);                                /* phase-separated */

	/* ---- (c) safety fallbacks only move BACKWARD ------------------------- */

	/* grant-window expiry with NO unicast -> back to PROBING, never ESTABLISHED */
	reac_master_init(&m, SRC, FPS);
	cnt = 0;
	slot(&m, NULL, &cnt);                        /* IDLE -> PROBING on first slot */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	for (int i = 0; i < m.grant_frames + 8; i++) {
		enum reac_master_emit e = slot(&m, NULL, &cnt);
		CHK(m.state != REAC_M_ESTABLISHED);
		(void)e;
	}
	CHK(m.state == REAC_M_PROBING);
	CHK(m.drop_reason == REAC_M_DROP_GRANT_TIMEOUT);
	CHK(m.grant_attempts == 1);

	/* established then 600 slots without RX -> peer-gone, at exactly 600 */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 1);
	CHK(m.state == REAC_M_ESTABLISHED);
	for (int i = 0; i < REAC_M_LINKCHECK_RELOAD - 1; i++)
		slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_ESTABLISHED);          /* 599 silent slots: still held */
	slot(&m, NULL, &cnt);
	CHK(m.state == REAC_M_PROBING);              /* the 600th drains the budget */
	CHK(m.drop_reason == REAC_M_DROP_PEER_GONE);

	/* established + explicit BYE (hb selector 0x00) -> PROBING immediately */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 1);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_BYE, BOX, NULL) == 1);
	CHK(m.state == REAC_M_PROBING && m.drop_reason == REAC_M_DROP_BYE);

	/* JOIN from a second MAC while established -> mac-change, re-grant the new */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL) == 1);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.drop_reason == REAC_M_DROP_MAC_CHANGE);
	CHK(memcmp(m.box_mac, BOX2, 6) == 0);        /* latched the new box */

	/* a fresh JOIN mid-window restarts the window */
	for (int i = 0; i < m.grant_frames / 2; i++)
		slot(&m, NULL, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 0);
	CHK(m.grant_ticks == 0 && m.state == REAC_M_GRANTING);

	printf("OK: master byte oracle (chanmap/announce-with-our-MAC/grant-echo/probe) + "
	       "event-driven establishment (no timer forward path; presence never grants; "
	       "JOIN->GRANT-echo->first-unicast->ESTABLISHED; backward-only fallbacks; "
	       "dual 1/s streams; free-running counter)\n");
	return 0;
}
