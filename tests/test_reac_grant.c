// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_grant — the master's per-channel ENROLLMENT SWEEP (GRANT-SWEEP.md).
 *
 * The bug this guards (live 2026-07-17): reac-pw replayed a grant burst transcribed
 * from an M-200 granting an S-0808 (8 inputs at base 0x00) at ANY box. Handed to a
 * 16-input S-1608 (base 0x20) the box LINKS and streams audio — and then ignores
 * every head-amp record addressing CH 0x20, because our own grant never claimed
 * those slots. 28 byte-perfect records in 40 s; 48V never lit. Link established !=
 * channels enrolled.
 *
 * So the load-bearing property is an AGREEMENT: the slots the sweep enrolls must be
 * the slots our head-amp traffic later addresses. These tests pin both halves —
 * the allocator that decides the slots, and the sweep generated over them.
 *
 * Pure: no socket, no FSM, no hardware. */
#include "reac_grant.h"
#include "reac_ctrl.h"
#include "reac_headamp_tx.h"
#include "reac_boxreg.h"   /* the AUDIO-fabric allocator — the other slot space */
#include "reac_slots.h"

#include <reac/reac.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#include "reac_grant_golden.inc"   /* a real M-200i granting a real S-1608 */

/* A sweep row's record fields, for readability. Row is [type|block] = frame[16:50],
 * so a frame offset f maps to row index f-16. */
#define ROW(f) ((f) - 16)
static int row_is_groupa(const uint8_t r[34]) { return r[ROW(32)] == 0x12 && r[ROW(33)] == 0x12 &&
                                                       r[ROW(34)] == 0x01 && r[ROW(35)] == 0x01; }
static int row_is_groupb(const uint8_t r[34]) { return r[ROW(32)] == 0x12 && r[ROW(33)] == 0x11; }
static uint8_t row_ch(const uint8_t r[34])    { return r[ROW(36)]; }
static uint8_t row_param(const uint8_t r[34]) { return r[ROW(37)]; }
static uint8_t row_value(const uint8_t r[34]) { return r[ROW(38)]; }

int main(void)
{
	/* ---------------------------------------------------------------- *
	 * 1. THE ALLOCATOR — the observed placements, byte-decoded off real desks
	 *    (GRANT-SWEEP.md): S-0808 8 in -> 0x00, S-1608 16 in -> 0x20,
	 *    S-4000S 32 in -> 0x00.
	 * ---------------------------------------------------------------- */
	struct reac_grant_alloc a;

	CHK(reac_grant_allocate(&a, 16) == 0);
	CHK(a.base == 0x20 && a.width == 16);   /* S-1608 */

	CHK(reac_grant_allocate(&a, 8) == 0);
	CHK(a.base == 0x00 && a.width == 8);    /* S-0808 */

	CHK(reac_grant_allocate(&a, 32) == 0);
	CHK(a.base == 0x00 && a.width == 32);   /* S-4000S — 0x20 would overrun */

	/* 1b. The 0x2f HEAD-AMP CEILING (REAC_HEADAMP_CEILING, reac_slots.h — NOT the
	 * 40-slot audio fabric). This is the rule that FORCES a 32-wide box to base at
	 * 0x00: at 0x20 it would run to 0x3f, past the ceiling. A per-width base
	 * constant would have happily allocated it there. */
	CHK(reac_grant_alloc_fits(0x20, 16) == 1);   /* 0x20..0x2f — exactly to the ceiling */
	CHK(reac_grant_alloc_fits(0x20, 32) == 0);   /* 0x20..0x3f — REJECTED */
	CHK(reac_grant_alloc_fits(0x00, 32) == 1);   /* 0x00..0x1f */
	CHK(reac_grant_alloc_fits(0x2f, 1)  == 1);   /* the ceiling slot itself */
	CHK(reac_grant_alloc_fits(0x30, 1)  == 0);   /* one past it */
	CHK(reac_grant_alloc_fits(0x29, 8)  == 0);   /* 0x29..0x30 — one past */
	CHK(reac_grant_alloc_fits(-1, 8)    == 0);
	CHK(reac_grant_alloc_fits(0, 0)     == 0);

	/* 1c. Unplaceable widths are refused rather than silently truncated. */
	CHK(reac_grant_allocate(&a, 0) == -1);
	CHK(reac_grant_allocate(&a, REAC_GRANT_MAX_WIDTH + 1) == -1);
	CHK(reac_grant_allocate(NULL, 8) == -1);

	/* 1d. A width with no observed placement falls back to the lowest base that
	 * fits — "width-many contiguous slots wherever they fit". */
	CHK(reac_grant_allocate(&a, 4) == 0);
	CHK(a.base == 0x00 && a.width == 4);

	/* ---------------------------------------------------------------- *
	 * 2. THE SWEEP SHAPE for an S-1608: 8 + 16*3 = 56 rows, matching the real
	 *    M-200 golden (matrix-m200-s1608-2026-07-11, deduped by frame counter)
	 *    frame-for-frame.
	 * ---------------------------------------------------------------- */
	uint8_t sweep[REAC_GRANT_SWEEP_MAX][34];
	struct reac_grant_alloc s1608 = { .base = 0x20, .width = 16 };
	int n = reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &s1608, NULL);
	CHK(n == 56);
	CHK(n == REAC_GRANT_SWEEP_LEN(16));

	/* 2a. Group A: width x 3 records, params 0/1/2 per channel, channels
	 * CONTIGUOUS from the allocated base and never past it. */
	int ga = 0, gb = 0;
	int param_seen[REAC_HEADAMP_SLOTS][REAC_HEADAMP_NPARAMS];
	memset(param_seen, 0, sizeof param_seen);
	for (int i = 0; i < n; i++) {
		if (row_is_groupa(sweep[i])) {
			ga++;
			uint8_t ch = row_ch(sweep[i]), p = row_param(sweep[i]);
			CHK(ch >= s1608.base && ch < s1608.base + s1608.width);
			CHK(p < REAC_HEADAMP_NPARAMS);
			param_seen[ch][p]++;
		} else if (row_is_groupb(sweep[i])) {
			gb++;
		}
	}
	CHK(ga == 16 * 3);
	CHK(gb == 6);
	for (int c = 0; c < REAC_HEADAMP_SLOTS; c++) {
		int in_box = (c >= s1608.base && c < s1608.base + s1608.width);
		for (int p = 0; p < REAC_HEADAMP_NPARAMS; p++)
			CHK(param_seen[c][p] == (in_box ? 1 : 0));   /* exactly [0,1,2], once each */
	}

	/* 2b. Widths scale, and each matches its golden's frame count exactly. */
	struct reac_grant_alloc s0808 = { .base = 0x00, .width = 8 };
	CHK(reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &s0808, NULL) == 32);
	struct reac_grant_alloc s4000 = { .base = 0x00, .width = 32 };
	CHK(reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &s4000, NULL) == 104);

	/* 2c. Bad args + capacity are refused, never truncated. */
	struct reac_grant_alloc over = { .base = 0x20, .width = 32 };   /* past the ceiling */
	CHK(reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &over, NULL) == -1);
	CHK(reac_grant_build_sweep(sweep, 10, &s1608, NULL) == -1);     /* too small */
	CHK(reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, NULL, NULL) == -1);

	/* ---------------------------------------------------------------- *
	 * 3. EVERY generated group-A record is BYTE-EXACT vs the proven builder
	 *    (reac_ctrl_build_headamp — the one already verified against a real
	 *    M-200 in tests/test_reac_headamp.c, both nested checksums included).
	 *    This is what makes "generated" safe: we did not hand-roll record bytes.
	 * ---------------------------------------------------------------- */
	n = reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &s1608, NULL);
	CHK(n == 56);
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t SRC[6]   = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 };
	for (int i = 0; i < n; i++) {
		if (!row_is_groupa(sweep[i]))
			continue;
		uint8_t ref[REAC_FRAME_BYTES];
		size_t len = reac_ctrl_build_headamp(ref, BCAST, SRC, 0x1234,
		                                     row_ch(sweep[i]), row_param(sweep[i]),
		                                     row_value(sweep[i]));
		CHK(len == REAC_FRAME_BYTES);
		/* The builder's [16:50] IS the sweep row: type + the whole control block,
		 * inner record checksum and outer block checksum alike. */
		CHK(memcmp(sweep[i], ref + 16, 34) == 0);
	}

	/* 3b. The outer block checksum holds on EVERY row (group A, group B and the
	 * two fixed head frames): sum over block [18:50] == 0 mod 256, so
	 * reac_master_stamp's re-checksum is a no-op and the wire bytes are a real
	 * M-200's. Row index r maps to frame offset r+16, so the block is rows [2:34]. */
	for (int i = 0; i < n; i++) {
		unsigned sum = 0;
		for (int b = 2; b < 34; b++)
			sum += sweep[i][b];
		CHK((sum & 0xff) == 0);
	}

	/* ---------------------------------------------------------------- *
	 * 4. VALUE SOURCING: the head-amp table when the cell is SET, the documented
	 *    safe default otherwise.
	 * ---------------------------------------------------------------- */
	/* 4a. With no table at all, every channel takes the default: phantom OFF, pad
	 * OFF, and a NON-ZERO SENS. The box only enrols a channel whose arming scene
	 * carries a real head-amp value; the old all-zero SENS default left every
	 * un-preset channel un-enrollable, so no later op-0403 write ever committed it.
	 * Phantom stays OFF — never default +48V. */
	uint8_t def_sens = reac_grant_headamp_value(NULL, 0x20, REAC_HEADAMP_SENS);
	CHK(def_sens != 0x00);   /* the enrol requirement: a real value, not minimum gain */
	for (int i = 0; i < n; i++) {
		if (!row_is_groupa(sweep[i]))
			continue;
		switch (row_param(sweep[i])) {
		case REAC_HEADAMP_PHANTOM: CHK(row_value(sweep[i]) == 0); break;
		case REAC_HEADAMP_PAD:     CHK(row_value(sweep[i]) == 0); break;
		case REAC_HEADAMP_SENS:    CHK(row_value(sweep[i]) == def_sens); break;
		}
	}
	/* The head-amp law is unchanged — 0x00 is still -10 dBu (minimum gain); we
	 * simply no longer DEFAULT there, because a channel must carry a real value to
	 * be enrolled by the box. */
	CHK(reac_headamp_sens_db(0x00, 0) == -10);

	/* 4b. Set cells are honoured; unset cells beside them still default. The
	 * channels used are 0x20 (the S-1608's input 1) and 0x2f (its input 16) —
	 * the latter being exactly the channel the old REAC_MAX_CHANNELS=40 bound
	 * silently rejected. */
	struct reac_headamp_tx tx;
	reac_headamp_tx_init(&tx, 8000);
	CHK(reac_headamp_tx_set(&tx, 0x20, REAC_HEADAMP_PHANTOM, 1) == 0);
	CHK(reac_headamp_tx_set(&tx, 0x20, REAC_HEADAMP_SENS, 0x07) == 0);
	CHK(reac_headamp_tx_set(&tx, 0x2f, REAC_HEADAMP_PAD, 1) == 0);

	n = reac_grant_build_sweep(sweep, REAC_GRANT_SWEEP_MAX, &s1608, &tx);
	CHK(n == 56);
	int checked = 0;
	for (int i = 0; i < n; i++) {
		if (!row_is_groupa(sweep[i]))
			continue;
		uint8_t ch = row_ch(sweep[i]), p = row_param(sweep[i]), v = row_value(sweep[i]);
		if (ch == 0x20 && p == REAC_HEADAMP_PHANTOM) { CHK(v == 1);    checked++; }
		if (ch == 0x20 && p == REAC_HEADAMP_SENS)    { CHK(v == 0x07); checked++; }
		if (ch == 0x20 && p == REAC_HEADAMP_PAD)     { CHK(v == 0);    checked++; }  /* unset */
		if (ch == 0x2f && p == REAC_HEADAMP_PAD)     { CHK(v == 1);    checked++; }
		if (ch == 0x2f && p == REAC_HEADAMP_PHANTOM) { CHK(v == 0);    checked++; }  /* unset */
		if (ch == 0x21 && p == REAC_HEADAMP_SENS)    { CHK(v == def_sens); checked++; }  /* unset -> real default */
		if (ch == 0x21 && p != REAC_HEADAMP_SENS)    { CHK(v == 0);        checked++; }  /* unset phantom/pad stay OFF */
	}
	CHK(checked == 8);   /* 5 named cells + the 3 params of ch 0x21 */

	/* 4c. A deliberate 0 is NOT an unset cell — `set` is the test, not the value.
	 * (Both read 0 here; the distinction matters the moment a default changes.) */
	CHK(reac_grant_headamp_value(&tx, 0x21, REAC_HEADAMP_PHANTOM) == 0);
	CHK(reac_headamp_tx_set(&tx, 0x21, REAC_HEADAMP_SENS, 0x00) == 0);
	CHK(tx.set[0x21][REAC_HEADAMP_SENS] == 1);
	CHK(reac_grant_headamp_value(&tx, 0x21, REAC_HEADAMP_SENS) == 0x00);

	/* 4d. The sourcing helper agrees with what the sweep actually emitted. */
	CHK(reac_grant_headamp_value(&tx, 0x20, REAC_HEADAMP_PHANTOM) == 1);
	CHK(reac_grant_headamp_value(&tx, 0x20, REAC_HEADAMP_SENS)    == 0x07);
	CHK(reac_grant_headamp_value(NULL, 0x20, REAC_HEADAMP_PHANTOM) == 0);
	CHK(reac_grant_headamp_value(&tx, 0x20, 0x03) == 0);   /* bad param */

	/* ---------------------------------------------------------------- *
	 * 5. GROUP B is the fixed, WIDTH-INVARIANT 6-record constant — byte-identical
	 *    across 8/16/32-input boxes (GRANT-SWEEP.md; re-verified against the
	 *    m200-s0808 and m200-s1608 goldens). Head-amp state must never leak into
	 *    it, and it must not scale with the allocation.
	 * ---------------------------------------------------------------- */
	uint8_t sw8[REAC_GRANT_SWEEP_MAX][34], sw16[REAC_GRANT_SWEEP_MAX][34],
	        sw32[REAC_GRANT_SWEEP_MAX][34];
	CHK(reac_grant_build_sweep(sw8,  REAC_GRANT_SWEEP_MAX, &s0808, &tx) == 32);
	CHK(reac_grant_build_sweep(sw16, REAC_GRANT_SWEEP_MAX, &s1608, &tx) == 56);
	CHK(reac_grant_build_sweep(sw32, REAC_GRANT_SWEEP_MAX, &s4000, &tx) == 104);

	uint8_t b8[6][34], b16[6][34], b32[6][34];
	int n8 = 0, n16 = 0, n32 = 0;
	for (int i = 0; i < 32;  i++) if (row_is_groupb(sw8[i]))  memcpy(b8[n8++],   sw8[i],  34);
	for (int i = 0; i < 56;  i++) if (row_is_groupb(sw16[i])) memcpy(b16[n16++], sw16[i], 34);
	for (int i = 0; i < 104; i++) if (row_is_groupb(sw32[i])) memcpy(b32[n32++], sw32[i], 34);
	CHK(n8 == 6 && n16 == 6 && n32 == 6);
	CHK(memcmp(b8, b16, sizeof b8) == 0);
	CHK(memcmp(b8, b32, sizeof b8) == 0);

	/* Group B's decoded records, verbatim from GRANT-SWEEP.md:
	 * (ch,sub,val) = (00,00,04) (06,00,08) (10,00,11) (10,11,09) (11,00,11) (11,11,09). */
	static const uint8_t B_EXPECT[6][3] = {
		{ 0x00, 0x00, 0x04 }, { 0x06, 0x00, 0x08 }, { 0x10, 0x00, 0x11 },
		{ 0x10, 0x11, 0x09 }, { 0x11, 0x00, 0x11 }, { 0x11, 0x11, 0x09 },
	};
	for (int i = 0; i < 6; i++) {
		CHK(b16[i][ROW(36)] == B_EXPECT[i][0]);
		CHK(b16[i][ROW(37)] == B_EXPECT[i][1]);
		CHK(b16[i][ROW(38)] == B_EXPECT[i][2]);
		CHK(b16[i][ROW(49)] == 0x03);   /* group B's trailer byte */
	}

	/* ---------------------------------------------------------------- *
	 * 6. THE ORDER, as measured on both real M-200 goldens:
	 *      HEAD_ACK | A[base].0/1/2 | HEAD_MARK | B x6 | A[base+1..].0/1/2
	 * ---------------------------------------------------------------- */
	CHK(!row_is_groupa(sw16[0]) && !row_is_groupb(sw16[0]));   /* HEAD_ACK  */
	for (int i = 1; i <= 3; i++) {
		CHK(row_is_groupa(sw16[i]));
		CHK(row_ch(sw16[i]) == 0x20 && row_param(sw16[i]) == i - 1);
	}
	CHK(!row_is_groupa(sw16[4]) && !row_is_groupb(sw16[4]));   /* HEAD_MARK */
	for (int i = 5; i <= 10; i++)
		CHK(row_is_groupb(sw16[i]));
	for (int i = 11; i < 56; i++) {
		CHK(row_is_groupa(sw16[i]));
		int k = i - 11;
		CHK(row_ch(sw16[i]) == 0x21 + k / 3);
		CHK(row_param(sw16[i]) == k % 3);
	}

	/* ---------------------------------------------------------------- *
	 * 7. THE GOLDEN: seeded with the head-amp state a real M-200i pushed to a real
	 *    S-1608, our GENERATED sweep must equal that desk's grant BYTE-FOR-BYTE —
	 *    all 56 frames, both nested checksums, in emission order.
	 *
	 *    This is the end-to-end proof of the 2026-07-17 discovery: group A is not
	 *    an opaque negotiation to be replayed, it is the head-amp channel strip
	 *    serialised. If that reading were wrong, feeding the decoded values back
	 *    through the head-amp builder could not possibly reconstruct the desk's
	 *    own bytes. It also proves the allocator picks the same slots the real
	 *    desk did (base 0x20), since a wrong base changes every group-A record.
	 * ---------------------------------------------------------------- */
	{
		struct reac_headamp_tx gtx;
		reac_headamp_tx_init(&gtx, 8000);
		for (size_t i = 0; i < sizeof GOLD_S1608_CELLS / sizeof GOLD_S1608_CELLS[0]; i++)
			CHK(reac_headamp_tx_set(&gtx, GOLD_S1608_CELLS[i][0],
			                        GOLD_S1608_CELLS[i][1],
			                        GOLD_S1608_CELLS[i][2]) == 0);

		struct reac_grant_alloc ga;
		CHK(reac_grant_allocate(&ga, 16) == 0);   /* the desk chose base 0x20 */

		uint8_t gen[REAC_GRANT_SWEEP_MAX][34];
		int gn = reac_grant_build_sweep(gen, REAC_GRANT_SWEEP_MAX, &ga, &gtx);
		CHK(gn == (int)(sizeof GOLD_S1608_SWEEP / sizeof GOLD_S1608_SWEEP[0]));
		for (int i = 0; i < gn; i++)
			CHK(memcmp(gen[i], GOLD_S1608_SWEEP[i], 34) == 0);
	}

	/* ---------------------------------------------------------------- *
	 * 5. THE TWO SLOT SPACES ARE NOT THE SAME SPACE (#69).
	 *
	 *    reac_slots.h defines both because the ONE ceiling this repo used to have
	 *    was named after the audio fabric and measured the head-amp space. Both
	 *    halves are load-bearing and they fail in OPPOSITE directions, so both are
	 *    pinned here, on the SAME span, side by side:
	 *
	 *      - AUDIO fabric = 40 slots. The master advertises it: cfea [17] = 0x28
	 *        (tests/test_reac_s1608.c's CAP_CFEA golden), and the ENROLL group map
	 *        spans exactly those 40 as 5 groups x 8. A box placed past slot 39 has
	 *        channels the downstream frame cannot carry — they go missing only once
	 *        real boxes are on the wire.
	 *      - HEAD-AMP space = 48 slots, 0x00..0x2f. Every desk in the corpus places
	 *        an S-1608 at base 0x20, and it is 16 wide, so its run reaches CH 47.
	 *        Clamping this to 40 makes that box's inputs 9..16 unaddressable — the
	 *        "48V never lit" bug class.
	 *
	 *    The span base 32 / width 16 is exactly where the two verdicts must differ:
	 *    LEGAL as head-amp, ILLEGAL as audio. Mutation-checked: widening
	 *    REAC_AUDIO_FABRIC_SLOTS to 48 fails 5b even with every named-constant
	 *    assertion below removed, and clamping the head-amp ceiling to 40 fails 5a.
	 * ---------------------------------------------------------------- */
	CHK(REAC_AUDIO_FABRIC_SLOTS == 40);      /* cfea [17] = 0x28              */
	CHK(REAC_HEADAMP_SLOTS      == 48);      /* CH 0x00..0x2f                 */
	CHK(REAC_HEADAMP_CEILING    == 0x2f);
	CHK(REAC_HEADAMP_SLOTS > REAC_AUDIO_FABRIC_SLOTS);   /* the whole point   */

	/* 5a. HEAD-AMP: an S-1608 based at 0x20 is legal THROUGH CH 47, and the sweep
	 * really does address that top channel with all three params. */
	CHK(reac_grant_alloc_fits(0x20, 16) == 1);
	{
		struct reac_grant_alloc s1608_top = { .base = 0x20, .width = 16 };
		uint8_t top[REAC_GRANT_SWEEP_MAX][34];
		int tn = reac_grant_build_sweep(top, REAC_GRANT_SWEEP_MAX, &s1608_top, NULL);
		CHK(tn == 56);
		int seen47 = 0, max_ch = 0;
		for (int i = 0; i < tn; i++) {
			if (!row_is_groupa(top[i]))
				continue;
			int ch = row_ch(top[i]);
			if (ch > max_ch)
				max_ch = ch;
			if (ch == 0x2f)
				seen47++;
		}
		CHK(max_ch == 0x2f);                   /* 47 — the box's input 16      */
		CHK(seen47 == REAC_HEADAMP_NPARAMS);   /* phantom + pad + sens         */
		CHK(max_ch >= REAC_AUDIO_FABRIC_SLOTS);  /* past the audio fabric, and
		                                          * correct — the whole finding */
	}

	/* 5b. AUDIO: the SAME span is refused by the audio allocator. reac_boxreg is
	 * where a box's audio is placed; a 16-wide box at base 32 would end at slot 47,
	 * past the 40 the frame carries, so both the pinned and the automatic path must
	 * refuse it. */
	{
		struct reac_boxreg r;
		reac_boxreg_init(&r, 0);                       /* 0 -> the full fabric */
		CHK(r.fabric == REAC_AUDIO_FABRIC_SLOTS);
		CHK(REAC_BOXREG_FABRIC == REAC_AUDIO_FABRIC_SLOTS);

		/* pinned at the S-1608's HEAD-AMP base: audio 32..47 — REFUSED */
		CHK(reac_boxreg_declare(&r, 16, "S-1608", 32) == -1);
		/* the last legal 16-wide audio placement is 24..39, one slot lower */
		CHK(reac_boxreg_declare(&r, 16, "S-1608", 24) >= 0);

		/* automatic allocation cannot cross 40 either: 32 + 16 = 48 slots asked
		 * of a 40-slot fabric, so the second box has nowhere to go. */
		struct reac_boxreg q;
		const uint8_t A[6] = { 0x00,0x40,0xab,0xc4,0x06,0x80 };
		const uint8_t B[6] = { 0x00,0x40,0xab,0xc4,0x80,0x3b };
		reac_boxreg_init(&q, 0);
		CHK(reac_boxreg_add(&q, A, 32) == 0);          /* S-4000S -> 0..31 */
		CHK(q.box[0].base == 0 && q.box[0].nch == 32);
		CHK(reac_boxreg_add(&q, B, 16) == -1);         /* would end at 47 — NO */
		CHK(reac_boxreg_add(&q, B, 8)  == 1);          /* 32..39 fits exactly */
		CHK(q.box[1].base == 32 && q.box[1].base + q.box[1].nch == 40);
	}

	printf("OK: grant enrollment sweep — allocator (8->0x00 16->0x20 32->0x00, 0x2f ceiling), "
	       "generated group A (width x 3, byte-exact vs the proven builder), "
	       "values from the head-amp table else safe defaults, group B fixed, "
	       "the seeded sweep is BYTE-IDENTICAL to a real M-200 x S-1608 grant (56 frames), "
	       "and the 48-slot head-amp space and the 40-slot audio fabric disagree on "
	       "base 32 x width 16 exactly as they must\n");
	return 0;
}
