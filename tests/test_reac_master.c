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
static const uint8_t GOLD_PROBE[34] =
 { 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xdd };
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

/* #130 fix 2: mirrors reac_master.c's FILLER_DESC_BYTE (the chosen placeholder
 * for the FILLER control-block descriptor a real master stamps there). */
#define FILLER_DESC_BYTE 0xdc

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
	struct reac_console_cfg s1608 = REAC_CONSOLE_CFG_S1608;
	reac_master_init(&m, SRC, &s1608, FPS);

	/* the S-1608 downstream is one chanmap frame (marker + 0x00..0x06). */
	CHK(m.chanmap_nframes == 1);

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

	/* 3. the grant is the ECHO of the received JOIN block, verbatim */
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	build_and_stamp(&m, f, REAC_M_EMIT_GRANT, 0, planar);
	CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x04 && f[19] == 0x03);
	CHK(memcmp(f + 18, ZONEA_JOIN, 32) == 0);            /* byte-for-byte echo */
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

	/* 5. the single chanmap frame lists the marker + channels 0x00..0x06. */
	{
		const uint8_t *blk = GOLD_CHANMAP + 2;   /* the 32-byte block */
		CHK(blk[5] == 0xfe);                     /* slot 0 = section marker */
		for (int c = 0; c <= 6; c++) {
			const uint8_t *t = blk + 5 + (c + 1) * 3;
			CHK(t[0] == (uint8_t)c && t[1] == 0x28 && t[2] == 0x00);
		}
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
			CHK(f[i + 1] == FILLER_DESC_BYTE);
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
	for (long i = 0; i < 60L * FPS; i++) {      /* 60 s of slots, zero RX */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		CHK((int)e >= 0);
		switch (e) {
		case REAC_M_EMIT_PROBE:    n_probe++; break;
		case REAC_M_EMIT_SUB01:    n_sub01++; break;
		case REAC_M_EMIT_SUB02:    n_sub02++; break;
		case REAC_M_EMIT_ANNOUNCE: n_ann++;   break;
		case REAC_M_EMIT_CHANMAP:  n_cm++;    CHK(idx == 0); break;
		case REAC_M_EMIT_GRANT:    n_grant++; break;
		case REAC_M_EMIT_FILLER:   break;
		}
	}
	CHK(m.state == REAC_M_PROBING);             /* NEVER advanced on a timer */
	CHK(n_grant == 0);                          /* invariant: NO grant without a validated JOIN */
	CHK(n_cm > 0);                              /* §4: chanmap advertised while unlinked */
	CHK(n_probe >= 108L * 60 && n_probe <= 120L * 60);   /* PROBE ~115/s */
	CHK(n_sub01 >= 58 && n_sub01 <= 62);        /* sub01 ~1/s */
	CHK(n_sub02 >= 58 && n_sub02 <= 62);        /* sub02 ~1/s */
	CHK(n_cm    >= 58 && n_cm    <= 62);        /* chanmap ~1/s */
	CHK(n_ann   >= 58 && n_ann   <= 62);        /* cfea ~1/s */

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

	/* 12 s established: the SAME continuous cadence runs while linked. Box RX
	 * every slot keeps HOLD loaded. */
	long e_cm = 0, e_ann = 0, e_s1 = 0, e_s2 = 0, e_pr = 0;
	long cm_slot = -1, ann_slot = -1;
	for (long i = 0; i < 12L * FPS; i++) {
		reac_master_rx(&m, REAC_M_RX_BOX_UNICAST, BOX, NULL);   /* upstream flood */
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		switch (e) {
		case REAC_M_EMIT_CHANMAP:
			e_cm++;
			CHK(idx == 0);
			if (cm_slot >= 0)
				CHK(i - cm_slot == FPS);            /* exactly 1/s */
			cm_slot = i;
			break;
		case REAC_M_EMIT_ANNOUNCE:
			e_ann++;
			if (ann_slot >= 0)
				CHK(i - ann_slot == FPS);           /* exactly 1/s */
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
	CHK(e_cm >= 11 && e_cm <= 13 && e_ann >= 11 && e_ann <= 13);   /* both every second */
	CHK(e_s1 >= 11 && e_s1 <= 13 && e_s2 >= 11 && e_s2 <= 13);     /* subs too */
	CHK(e_pr >= 108L * 12 && e_pr <= 120L * 12);                   /* PROBE ~115/s linked */
	CHK(cm_slot != ann_slot);                                      /* phase-separated */

	/* ---- (c) safety fallbacks only move BACKWARD ------------------------- */

	/* grant-window expiry with NO unicast -> back to PROBING, never ESTABLISHED */
	reac_master_init(&m, SRC, &s1608, FPS);
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

	printf("OK: master byte oracle (generated chanmap/cfea-with-our-MAC, fixed "
	       "probe/sub01/sub02, grant-echo) + event-driven establishment (no timer "
	       "forward path; presence never grants; JOIN->GRANT-echo->first-unicast->"
	       "ESTABLISHED; backward-only fallbacks; continuous 5-message cadence both "
	       "states; free-running counter)\n");
	return 0;
}
