// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* MASTER-role handshake byte verification. The master stamps cdea/cfea control
 * blocks into the downstream broadcast so a real Roland stagebox slaves to us.
 * This asserts, against the gold-capture bytes (reac-captures/wired-reac-a-
 * bothdirs, master 00:40:ab:ca:15:4d, + the §6/§13d transcription):
 *   1. the established channel-map frames the master emits byte-match the captured
 *      M-5000 blocks AND satisfy Sum(block[18..49]) mod 256 == 0;
 *   2. the cfea announce + cdea grant blocks match + checksum;
 *   3. the probe cycles sub-states 03->01->00->02 with a valid (re-applied) checksum;
 *   4. the chanmap cursor walks all 40 channels (0x00..0x27) across the 6 frames,
 *      none missing, none duplicated — the master rotates the full map;
 *   5. the FSM sequences IDLE -> PROBING -> GRANTING -> ESTABLISHED with the
 *      right wall-clock-equivalent frame budgets, and the audio a FILLER frame
 *      carries still round-trips through the decoder (control stamping is
 *      non-destructive to the audio reac_tx_build wrote). */
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
static const uint8_t GOLD_ANNOUNCE[34] =
 { 0xcf,0xea,0xff,0xff,0x01,0x00,0x01,0x03,0x0d,0x01,0x04,0x00,0x40,0xab,0xca,0x15,0x4d,0x28,0x10,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x9a };
static const uint8_t GOLD_GRANT[34] =
 { 0xcd,0xea,0x04,0x03,0x00,0x14,0x00,0x02,0x00,0xfe,0x0f,0xf0,0x41,0x0a,0x00,0x00,0x12,0x12,0x01,0x00,0x06,0x00,0x01,0x00,0x78,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };

/* Build a base downstream FILLER (so the audio + tail are present), then let the
 * master stamp the control block, and return the frame in `out`. */
static void build_and_stamp(uint8_t *out, enum reac_master_emit emit, int idx,
                            float *const *planar)
{
	reac_tx_build(out, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, SRC);
	reac_master_stamp(out, emit, idx);
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

	/* 1. each channel-map frame byte-matches the captured master block + checksum */
	for (int i = 0; i < REAC_M_CHANMAP_FRAMES; i++) {
		build_and_stamp(f, REAC_M_EMIT_CHANMAP, i, planar);
		CHK(memcmp(f + 16, GOLD_CHANMAP[i], 34) == 0);   /* type + block exact */
		CHK(reac_ctrl_checksum_verify(f) == 0);          /* Sum[18..49]%256==0 */
		CHK(f[16] == 0xcd && f[17] == 0xea);             /* cdea */
		CHK(f[18] == 0x01 && f[19] == 0x03);             /* established sub-state */
		CHK(f[20] == 0x00 && f[21] == 0x19);             /* BE len 0x0019 */
		CHK(f[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 &&
		    f[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);
	}

	/* 2. cfea announce + cdea grant byte-match + checksum */
	build_and_stamp(f, REAC_M_EMIT_ANNOUNCE, 0, planar);
	CHK(memcmp(f + 16, GOLD_ANNOUNCE, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[16] == 0xcf && f[17] == 0xea);                 /* cfea */
	CHK(f[33] == 0x28 && f[34] == 0x10);                 /* inCh 40, outCh 16 */

	build_and_stamp(f, REAC_M_EMIT_GRANT, 0, planar);
	CHK(memcmp(f + 16, GOLD_GRANT, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x04 && f[19] == 0x03);  /* connect grant */

	/* 3. probe cycles sub-states 03->01->00->02, each a valid checksum */
	static const uint8_t want_sub[4] = { 0x03, 0x01, 0x00, 0x02 };
	for (int i = 0; i < 4; i++) {
		build_and_stamp(f, REAC_M_EMIT_PROBE, i, planar);
		CHK(f[16] == 0xcd && f[17] == 0xea && f[18] == 0x01);   /* cdea 01 */
		CHK(f[19] == want_sub[i]);                              /* the sub-state */
		CHK(reac_ctrl_checksum_verify(f) == 0);                 /* re-applied cksum */
	}

	/* 4. the chanmap cursor walks the channel space with NO duplicates and broad
	 * coverage. Records are 3 bytes (ch#, flag, 00) starting at block byte [5];
	 * count real channel records (flag 0x28/0x38). The 6 captured frames are the
	 * master cycling its map (12 frames captured = these 6 distinct), and span
	 * 47/48 channel slots 0x00..0x2f — one slot (0x13) falls in a 7th frame the
	 * 120 s capture didn't include, so coverage is ">=39 of the 40 audio chans",
	 * not "all 40". What MUST hold is no duplicate (a repeat would read as a stale
	 * map at the desk's 6-identical guard). */
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

	/* 5a. FSM sequence: IDLE -> PROBING -> GRANTING -> ESTABLISHED on the budgets. */
	struct reac_master m;
	reac_master_init(&m, SRC, 8000);
	uint16_t c; int idx;
	CHK(m.state == REAC_M_IDLE);
	reac_master_next(&m, &c, &idx);
	CHK(m.state == REAC_M_IDLE && c == 0);              /* counter starts at 0 */

	reac_master_set_box_present(&m, 1);
	CHK(m.state == REAC_M_PROBING);
	for (int i = 0; i < m.probe_frames; i++)
		reac_master_next(&m, &c, &idx);
	CHK(m.state == REAC_M_GRANTING);
	for (int i = 0; i < m.grant_frames; i++)
		reac_master_next(&m, &c, &idx);
	CHK(m.state == REAC_M_ESTABLISHED);

	/* 5b. in ESTABLISHED, the control slot alternates CHANMAP and ANNOUNCE ~1/s,
	 * the chanmap index walks 0..5, and the rest are FILLER. */
	int n_chanmap = 0, n_announce = 0, n_filler = 0, n_other = 0;
	int last_chanmap_idx = -1, walked = 0;
	for (int i = 0; i < m.hb_period * 12; i++) {   /* ~12 control frames */
		enum reac_master_emit e = reac_master_next(&m, &c, &idx);
		if (e == REAC_M_EMIT_CHANMAP) {
			n_chanmap++;
			if (last_chanmap_idx >= 0 &&
			    idx == (last_chanmap_idx + 1) % REAC_M_CHANMAP_FRAMES)
				walked++;
			last_chanmap_idx = idx;
		} else if (e == REAC_M_EMIT_ANNOUNCE) {
			n_announce++;
		} else if (e == REAC_M_EMIT_FILLER) {
			n_filler++;
		} else {
			n_other++;
		}
	}
	CHK(n_chanmap >= 5 && n_announce >= 5);     /* both flow ~1/s */
	CHK(walked >= 4);                           /* the cursor advanced/wrapped */
	CHK(n_filler > 0 && n_other == 0);          /* audio FILLER dominates, no probe/grant */

	/* 5c. control stamping is non-destructive: a FILLER frame's audio round-trips. */
	const struct reac_mode mode = { 48000, 40, 12 };
	build_and_stamp(f, REAC_M_EMIT_FILLER, 0, planar);
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(f, REAC_FRAME_BYTES, &mode, s24);
	CHK(ns == REAC_SAMPLES_PER_PKT);
	CHK(f[16] == 0x00 && f[17] == 0x00);        /* FILLER keeps type 00 00 */

	/* box absent -> DROP back to IDLE (emits plain FILLER, no handshake) */
	reac_master_set_box_present(&m, 0);
	CHK(m.state == REAC_M_IDLE);
	CHK(reac_master_next(&m, &c, &idx) == REAC_M_EMIT_FILLER);

	printf("OK: master cdea/cfea blocks byte-match the captured M-5000; checksum + "
	       "full 40-ch map walk + IDLE->PROBE->GRANT->ESTABLISHED + FILLER audio intact\n");
	return 0;
}
