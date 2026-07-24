// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_grant.h"
#include "reac_ctrl.h"        /* reac_ctrl_stamp_headamp, enum reac_headamp_param */
#include "reac_headamp_tx.h"  /* struct reac_headamp_tx */

#include <reac/reac.h>        /* REAC_FRAME_BYTES */
#include <string.h>

/* ---- The allocator ------------------------------------------------------- *
 *
 * base(width) + width, one DERIVED RULE (not a per-model table), wire-proven
 * byte-for-byte against the goldens in reac-captures/captures/:
 *
 *   box      inputs  base   group-A slots  golden
 *   S-0808     8      0x00   0x00..0x07     matrix-m200-s0808-2026-07-11
 *   S-1608    16      0x20   0x20..0x2f     matrix-m200-s1608-2026-07-11
 *   S-4000S   32      0x00   0x00..0x1f     matrix-m200-s4000-2026-07-24 + matrix-m5000-s4000-unit{1,2}
 *
 * THE RULE: base = (width == 16) ? 0x20 : 0x00. The S-1608 firmware sits its 16
 * inputs at fabric offset 0x20; every other Roland width bases at 0x00. This is
 * console-generation-INDEPENDENT — the S-4000S bases at 0x00 under both a real M-200
 * (matrix-m200-s4000-2026-07-24) and a real M-5000, which settled what the earlier
 * three-point set (one S-4000S golden, from an M-5000) could not. It is the SAME rule
 * reac_slave.c uses to DECLARE its base (ch_base = box_channels==16 ? 0x20 : 0x00),
 * and it equals the model's head-amp CH base baked into the rest of the stack
 * (reac_ctrl.h, openmixer) — so master-read, slave-declare and head-amp all derive
 * from ONE width value. 24 (S-2416) falls out with no new case: base 0x00, slots
 * 0x00..0x17, frame 916 B. The base+width travel to the box in the grant (no hardwired
 * base to match); the 0x2f fabric ceiling is real (a 32-wide box cannot base at 0x20 —
 * it would run to 0x3f), gated by reac_grant_alloc_fits.
 *
 * reac-pw grants ONE box at a time; when several boxes must share the fabric (#129)
 * this becomes a free-list and the base rule becomes each box's placement preference. */


int reac_grant_alloc_fits(int base, int width)
{
	if (width <= 0 || width > REAC_GRANT_MAX_WIDTH)
		return 0;
	if (base < 0)
		return 0;
	/* The ceiling test, stated as the doc states it: the LAST slot the box would
	 * own is base+width-1, and it must not pass 0x2f. */
	return (base + width - 1) <= REAC_GRANT_FABRIC_CEILING;
}

int reac_grant_allocate(struct reac_grant_alloc *out, int in_ch)
{
	if (!out || in_ch <= 0 || in_ch > REAC_GRANT_MAX_WIDTH)
		return -1;

	/* base(width): the S-1608's 16 inputs sit at fabric 0x20, every other width at
	 * 0x00 (see the block comment; the SAME rule as reac_slave.c's ch_base). fits()
	 * gates the 0x2f ceiling — it forbids a 32-wide box taking 0x20 (would run to
	 * 0x3f) and keeps any width from allocating past the fabric. */
	int base = (in_ch == 16) ? 0x20 : 0x00;
	if (!reac_grant_alloc_fits(base, in_ch))
		return -1;
	out->base  = (uint8_t)base;
	out->width = (uint8_t)in_ch;
	return 0;
}

/* ---- The group-A values -------------------------------------------------- *
 *
 * DEFAULTS for a channel the operator has not configured. Two of the three stay
 * safe-off; SENS is deliberately NON-ZERO, and that is the whole fix for the
 * multi-week "head-amp never commits on a real box" chase:
 *
 *   phantom = 0 (OFF). Never default +48V on. Phantom into a ribbon mic or an
 *       unbalanced line source can destroy it, and a box we have just enrolled is
 *       by definition a box whose patch we do not yet know. The one direction that
 *       is never recoverable is the one we must not take by default.
 *   pad     = 0 (OFF). A -20 dB attenuator the operator adds when a hot source
 *       clips; clipping is recoverable, so off is the safe default.
 *   sens    = REAC_GRANT_DEFAULT_SENS, NON-ZERO. The box only ENROLS a channel
 *       whose arming scene carries a REAL (non-zero) head-amp value. A channel
 *       armed all-zero is never enrolled, so no later op-0403 write — however
 *       byte-perfect — ever commits it. An all-zero SENS default (what this was)
 *       therefore silently disabled head-amp control on every un-preset channel:
 *       the box ignored phantom/pad/sens forever. Operator-confirmed on the S-0808
 *       (2026-07-23): a real-valued arming scene lit BOTH condensers; the all-zero
 *       scene stayed dead. So we arm a real value. phantom stays OFF, so a non-zero
 *       SENS cannot drive 48V; at worst a hot source clips and the operator pads it.
 *       0x20 = -42 dBu pad-off (reac_headamp_sens_db) — moderate gain, and a value
 *       the M-200 desk itself arms (its input 1), demonstrably one a box enrolls.
 *
 * OPEN (pending live S-1608 confirmation): whether SENS-alone with phantom=0 is
 * enough to enrol, and the minimal enrolling value. Until proven, arm a known-good
 * real value rather than probe for the floor.
 */
#define REAC_GRANT_DEFAULT_PHANTOM 0x00
#define REAC_GRANT_DEFAULT_PAD     0x00
#define REAC_GRANT_DEFAULT_SENS    0x20

static uint8_t default_for(uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM: return REAC_GRANT_DEFAULT_PHANTOM;
	case REAC_HEADAMP_PAD:     return REAC_GRANT_DEFAULT_PAD;
	case REAC_HEADAMP_SENS:    return REAC_GRANT_DEFAULT_SENS;
	default:                   return 0;
	}
}

uint8_t reac_grant_headamp_value(const struct reac_headamp_tx *tx,
                                 uint8_t ch, uint8_t param)
{
	if (param >= REAC_HEADAMP_NPARAMS)
		return 0;
	/* The table is the operator's/config's intent; `set` is what distinguishes a
	 * deliberate 0 from an unset cell, so it — not the value — is the test. */
	if (tx && ch < REAC_HEADAMP_MAX_CH && tx->set[ch][param])
		return tx->value[ch][param];
	return default_for(param);
}

/* ---- The fixed scaffolding ----------------------------------------------- *
 * Both goldens (m200 x S-0808 and m200 x S-1608) open the burst with the same two
 * cdea 04 03 0014 head frames and carry the same 6 group-B records, byte-identical
 * across box widths. Only group A scales. Deduped by frame counter from
 * matrix-m200-s{0808,1608}-2026-07-11.pcap (the mirror tap duplicates every frame;
 * see the ordering note below). Each row already sums to 0 over [18:50], so
 * reac_master_stamp's checksum re-stamp is a no-op and the on-wire bytes equal a
 * real M-200's. */

/* cdea 04 03 0014, record 12 12 01 00: the master's ACK of the box's join params. */
static const uint8_t GRANT_HEAD_ACK[34] = {
	0xcd, 0xea, 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0,
	0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00,
	0x78, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* cdea 04 03 0014, record 12 12 00 00: the marker that separates the first
 * channel's group-A records from the group-B block. */
static const uint8_t GRANT_HEAD_MARK[34] = {
	0xcd, 0xea, 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0,
	0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x7d, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* GROUP B — the fixed 6-record constant (marker 12 11, TAG 05 00), byte-identical
 * across 8/16/32-input boxes: (ch,sub,val) = (00,00,04) (06,00,08) (10,00,11)
 * (10,11,09) (11,00,11) (11,11,09). Unchanged from the tables it replaces. */
#define REAC_GRANT_GROUPB_LEN 6
static const uint8_t GRANT_GROUPB[REAC_GRANT_GROUPB_LEN][34] = {
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x00, 0x00, 0x04, 0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x06, 0x00, 0x08, 0x6d, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x10, 0x00, 0x11, 0x5a, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x10, 0x11, 0x09, 0x51, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x11, 0x00, 0x11, 0x59, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
	{ 0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x11, 0x05, 0x00, 0x11, 0x11, 0x09, 0x50, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 },
};

/* Emit ONE group-A record via the proven head-amp builder. We build into a scratch
 * frame and lift its [16:50] rather than re-deriving the record bytes: that keeps
 * reac_ctrl_stamp_headamp — the function already byte-verified against a real
 * M-200, including BOTH nested checksums — as the single source of these bytes. */
static int put_groupa(uint8_t row[34], uint8_t ch, uint8_t param, uint8_t value)
{
	uint8_t scratch[REAC_FRAME_BYTES];
	memset(scratch, 0, sizeof scratch);
	if (reac_ctrl_stamp_headamp(scratch, ch, param, value) != 0)
		return -1;
	memcpy(row, scratch + 16, 34);   /* [16:50] = type[2] + control block[32] */
	return 0;
}

int reac_grant_build_sweep(uint8_t sweep[][34], int max,
                           const struct reac_grant_alloc *alloc,
                           const struct reac_headamp_tx *tx)
{
	if (!sweep || !alloc)
		return -1;
	int w = alloc->width;
	if (!reac_grant_alloc_fits(alloc->base, w))
		return -1;
	/* Every slot we are about to address must be a real head-amp channel — the
	 * fabric ceiling already guarantees this, but assert it against the head-amp
	 * channel space too so the two can never drift apart unnoticed. */
	if (alloc->base + w > REAC_HEADAMP_MAX_CH)
		return -1;
	int n = REAC_GRANT_SWEEP_LEN(w);
	if (max < n)
		return -1;

	/* THE ORDER, measured on both real M-200 goldens (deduped by frame counter):
	 *
	 *   HEAD_ACK | A[base].0 A[base].1 A[base].2 | HEAD_MARK | B x6 |
	 *   A[base+1].0..2 | A[base+2].0..2 | ... | A[base+w-1].0..2
	 *
	 * i.e. the FIRST allocated channel's three head-amp records ride up front,
	 * bracketed by the two 0014 head frames, with group B wedged between them and
	 * the rest of group A. 8 + w*3 frames: 32 for an S-0808, 56 for an S-1608 —
	 * frame-for-frame the shape of the tables this replaces.
	 *
	 * ONE-SHOT BURST, not a periodic stream (GRANT-SWEEP.md: "MEASURED SHAPE — do
	 * not 'correct' to a periodic stream"). The caller (reac_master) spaces these
	 * rows one per grant_stride slots and then goes calm.
	 *
	 * NOT x2 REPEATS. The captures show 96 group-A frames for a 16-input box, and
	 * the RE notes read that as "16 x 3 x 2 repeats". It is not: the pairs carry an
	 * IDENTICAL frame counter (bytes 14-15, which a real master increments every
	 * slot) and differ only in captured length (1494 vs 1492 = the +2 FCS). They are
	 * ONE frame seen twice by the switch mirror — the same mirror artifact this repo
	 * already documents for the "1494 B OHRCA frame" (reac_tx.h, #156). Deduped by
	 * counter, both goldens hold exactly 48 unique group-A records, one each. Emitting
	 * each record twice would be reproducing a capture artifact.
	 */
	int k = 0;
	memcpy(sweep[k++], GRANT_HEAD_ACK, 34);
	for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
		if (put_groupa(sweep[k++], alloc->base, p,
		               reac_grant_headamp_value(tx, alloc->base, p)) != 0)
			return -1;
	memcpy(sweep[k++], GRANT_HEAD_MARK, 34);
	for (int i = 0; i < REAC_GRANT_GROUPB_LEN; i++)
		memcpy(sweep[k++], GRANT_GROUPB[i], 34);
	for (int c = 1; c < w; c++) {
		uint8_t ch = (uint8_t)(alloc->base + c);
		for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
			if (put_groupa(sweep[k++], ch, p,
			               reac_grant_headamp_value(tx, ch, p)) != 0)
				return -1;
	}
	return k;
}
