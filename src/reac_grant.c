// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_grant.h"
#include "reac_ctrl.h"        /* reac_ctrl_stamp_headamp, enum reac_headamp_param */
#include "reac_headamp_tx.h"  /* struct reac_headamp_tx */

#include <reac/reac.h>        /* REAC_FRAME_BYTES */
#include <string.h>

/* ---- The allocator ------------------------------------------------------- *
 *
 * EVIDENCE (GRANT-SWEEP.md + the goldens in reac-captures/captures/, all decoded
 * byte-for-byte):
 *
 *   box      inputs  observed base  group-A slots  golden
 *   S-0808     8        0x00          0x00..0x07   matrix-m200-s0808-2026-07-11
 *   S-1608    16        0x20          0x20..0x2f   matrix-m200-s1608-2026-07-11
 *   S-4000S   32        0x00          0x00..0x1f   matrix-m5000-s4000-unit{1,2}
 *
 * WHAT THE EVIDENCE DOES AND DOES NOT SETTLE. It settles that base+width are the
 * MASTER's decision and travel to the box in the grant (so the box has no hardwired
 * base to match), and that the 0x2f ceiling is real (a 32-wide box CANNOT base at
 * 0x20 — it would run to 0x3f). It does NOT settle a derivable placement LAW: three
 * points across two different desk models admit no unique rule (lowest-fit predicts
 * 0x00 for the S-1608 and is wrong; top-aligned predicts 0x28 for the S-0808 and is
 * wrong). GRANT-SWEEP.md's "width-many contiguous slots wherever they fit" is a
 * description of the freedom, not of the choice.
 *
 * SO: we pin the OBSERVED base per width rather than invent a law. Reasons, in
 * order: (a) it is the only placement each real box is known to have accepted;
 * (b) the S-1608's 0x20 origin is already baked into the rest of the stack as that
 * model's head-amp CH base (reac_ctrl.h's head-amp block comment, openmixer's
 * channel mapping), so choosing differently here would silently desync them;
 * (c) a wrong-but-self-consistent allocation is exactly the failure we are fixing —
 * being consistent with the REST OF THE WORLD is the whole point.
 *
 * This is a POLICY table, deliberately separated from the mechanism below it, so
 * multi-box allocation (#129 — several boxes sharing one fabric) can replace the
 * policy without touching the sweep generator. Today reac-pw grants ONE box at a
 * time, so a static policy is honest; the day two boxes must coexist, this becomes
 * a real free-list over the fabric and the observed bases become preferences. */
struct grant_placement {
	uint8_t width;
	uint8_t base;
};

static const struct grant_placement OBSERVED_PLACEMENT[] = {
	{  8, 0x00 },   /* S-0808  */
	{ 16, 0x20 },   /* S-1608  */
	{ 32, 0x00 },   /* S-4000S */
};

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

	/* The observed base for this width, when we have one AND it still fits. The
	 * fits() gate is not ceremony: it is what forbids a 32-wide box from taking
	 * the S-1608's 0x20, and it keeps a future policy edit from silently
	 * allocating past the fabric. */
	for (size_t i = 0; i < sizeof OBSERVED_PLACEMENT / sizeof OBSERVED_PLACEMENT[0]; i++) {
		if (OBSERVED_PLACEMENT[i].width != in_ch)
			continue;
		if (!reac_grant_alloc_fits(OBSERVED_PLACEMENT[i].base, in_ch))
			break;                       /* observed base no longer placeable */
		out->base  = OBSERVED_PLACEMENT[i].base;
		out->width = (uint8_t)in_ch;
		return 0;
	}

	/* An unobserved width (or an observed base that does not fit): take the lowest
	 * base that does. "Width-many contiguous slots wherever they fit", with the
	 * fabric otherwise empty — reac-pw grants one box at a time (see #129). */
	if (!reac_grant_alloc_fits(0, in_ch))
		return -1;
	out->base  = 0x00;
	out->width = (uint8_t)in_ch;
	return 0;
}

/* ---- The group-A values -------------------------------------------------- *
 *
 * SAFE DEFAULTS for a channel the operator has not configured:
 *
 *   phantom = 0 (OFF). Never default +48V on. Phantom into a ribbon mic or an
 *       unbalanced line source can destroy it, and a box we have just enrolled is
 *       by definition a box whose patch we do not yet know. The one direction that
 *       is never recoverable is the one we must not take by default.
 *   pad     = 0 (OFF). The pad is a -20 dB attenuator; leaving it off keeps the
 *       SENS default below meaningful in its own (pad-off) reference frame, and a
 *       pad is trivially added by the operator when a hot source clips.
 *   sens    = 0x00. Per the head-amp law (reac_headamp_sens_db):
 *       dBu = -10 - value + (pad ? 20 : 0), so value 0x00 with pad off is -10 dBu
 *       — the LEAST sensitive setting the preamp offers, i.e. MINIMUM gain. An
 *       unknown source therefore cannot arrive pre-amplified into clipping or into
 *       a feedback howl; the operator opens the gain deliberately, which is the
 *       direction that is safe to be wrong in. The real M-200 golden itself pushes
 *       0x00 on several channels (0x24, 0x25), so this is a value a live desk
 *       demonstrably enrolls with — not a value we invented.
 */
#define REAC_GRANT_DEFAULT_PHANTOM 0x00
#define REAC_GRANT_DEFAULT_PAD     0x00
#define REAC_GRANT_DEFAULT_SENS    0x00

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
