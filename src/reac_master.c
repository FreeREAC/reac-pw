// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_master.h"
#include "reac_ctrl.h"   /* reac_ctrl_checksum_apply, REAC_CTRL_* offsets */

#include <reac/reac.h>   /* REAC_FRAME_BYTES, REAC_END_MARKER_*, ... */
#include <string.h>

/* ------------------------------------------------------------------------- *
 * Captured master control blocks (the byte source-of-truth).
 *
 * Each array is the 34 bytes [16:50] of a real master broadcast frame: the
 * 2 type bytes [16:18] (cd ea / cf ea) followed by the 32-byte control block
 * [18:50]. Sum(block[18..49]) mod 256 == 0 holds on every one (verified against
 * reac-captures + the §6/§13d transcription). We stamp these verbatim and only
 * re-stamp the live counter at [14:16] of the frame, so the bytes a real desk
 * sees match the M-5000 exactly.
 *
 *   CHANMAP_0..5 — the established channel-map walk (cdea 01 03 0019 ...28...),
 *                  6 frames covering chans 0x00..0x2f in groups of 8, captured
 *                  from master 00:40:ab:ca:15:4d
 *                  (reac-captures/wired-reac-a-bothdirs-2026-06-09.pcap).
 *   ANNOUNCE     — the cfea master announce (inCh 0x28=40, outCh 0x10=16), same
 *                  capture. The master MAC embedded in the cfea payload is the
 *                  captured desk's; a real box learns the master from the L2
 *                  source (FSM doc), so this is informational, kept for fidelity.
 *   PROBE_BLK    — cdea 01 SUB 001a (hunting), from §6. Byte [3] is the live sub-
 *                  state, cycled 03->01->00->02 with the checksum re-applied.
 *   GRANT_BLK    — cdea 04 03 0014 0002 00 fe ... (the connect-grant), from §6/§13d.
 * ------------------------------------------------------------------------- */

static const uint8_t CHANMAP_0[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x14, 0x28, 0x00, 0x15, 0x28, 0x00, 0x16, 0x28, 0x00, 0x17, 0x28, 0x00, 0x18, 0x28, 0x00, 0x19, 0x28, 0x00, 0x1a, 0x28, 0x00, 0x1b, 0x28, 0x00, 0x00, 0x00, 0xe6 };
static const uint8_t CHANMAP_1[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x1c, 0x28, 0x00, 0x1d, 0x28, 0x00, 0x1e, 0x28, 0x00, 0x1f, 0x28, 0x00, 0x20, 0x28, 0x00, 0x21, 0x28, 0x00, 0x22, 0x28, 0x00, 0x23, 0x28, 0x00, 0x00, 0x00, 0xa6 };
static const uint8_t CHANMAP_2[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x24, 0x28, 0x00, 0x25, 0x28, 0x00, 0x26, 0x28, 0x00, 0x27, 0x28, 0x00, 0x28, 0x38, 0x00, 0x29, 0x38, 0x00, 0x2a, 0x38, 0x00, 0x2b, 0x38, 0x00, 0x00, 0x00, 0x26 };
static const uint8_t CHANMAP_3[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x2c, 0x38, 0x00, 0x2d, 0x38, 0x00, 0x2e, 0x38, 0x00, 0x2f, 0x38, 0x00, 0xfe, 0x01, 0x00, 0x00, 0x28, 0x00, 0x01, 0x28, 0x00, 0x02, 0x28, 0x00, 0x00, 0x00, 0xd2 };
static const uint8_t CHANMAP_4[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x03, 0x28, 0x00, 0x04, 0x28, 0x00, 0x05, 0x28, 0x00, 0x06, 0x28, 0x00, 0x07, 0x28, 0x00, 0x08, 0x28, 0x00, 0x09, 0x28, 0x00, 0x0a, 0x28, 0x00, 0x00, 0x00, 0x6e };
static const uint8_t CHANMAP_5[34] = { 0xcd, 0xea, 0x01, 0x03, 0x00, 0x19, 0x01, 0x0b, 0x28, 0x00, 0x0c, 0x28, 0x00, 0x0d, 0x28, 0x00, 0x0e, 0x28, 0x00, 0x0f, 0x28, 0x00, 0x10, 0x28, 0x00, 0x11, 0x28, 0x00, 0x12, 0x28, 0x00, 0x00, 0x00, 0x2e };

static const uint8_t *const CHANMAP[REAC_M_CHANMAP_FRAMES] = {
	CHANMAP_0, CHANMAP_1, CHANMAP_2, CHANMAP_3, CHANMAP_4, CHANMAP_5,
};

static const uint8_t ANNOUNCE_BLK[34] = { 0xcf, 0xea, 0xff, 0xff, 0x01, 0x00, 0x01, 0x03, 0x0d, 0x01, 0x04, 0x00, 0x40, 0xab, 0xca, 0x15, 0x4d, 0x28, 0x10, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9a };
static const uint8_t PROBE_BLK[34]    = { 0xcd, 0xea, 0x01, 0x00, 0x00, 0x1a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc };
static const uint8_t GRANT_BLK[34]    = { 0xcd, 0xea, 0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe, 0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Probe sub-state cycle (wire order from §13d: 03->01->00->02, 00 dominates the
 * loop but we cycle one per control slot so the box sees all four). */
const uint8_t REAC_M_PROBE_SUBSTATES[4] = { 0x03, 0x01, 0x00, 0x02 };

void reac_master_init(struct reac_master *m, const uint8_t src[6], int fps)
{
	memset(m, 0, sizeof *m);
	m->state = REAC_M_IDLE;
	memcpy(m->src, src, 6);
	m->fps = fps > 0 ? fps : 8000;

	/* Frame-count budgets scale with the rate so the wall-clock cadence (~1 s
	 * probe, ~150 ms grant, ~1/s heartbeat) is the same at 44.1/48/96 k. */
	m->probe_frames = m->fps;                 /* ~1 s */
	m->grant_frames = (m->fps * 15) / 100;    /* ~150 ms */
	if (m->grant_frames < 1)
		m->grant_frames = 1;
	m->hb_period = m->fps;                     /* ~1 control frame per second */
}

void reac_master_set_box_present(struct reac_master *m, int present)
{
	if (present) {
		if (m->state == REAC_M_IDLE) {
			m->state = REAC_M_PROBING;
			m->state_ticks = 0;
			m->hb_tick = 0;
			m->probe_sub_idx = 0;
		}
	} else {
		m->state = REAC_M_IDLE;
		m->state_ticks = 0;
	}
}

enum reac_master_emit reac_master_next(struct reac_master *m, uint16_t *counter,
                                       int *tmpl_idx)
{
	*counter = m->counter;
	m->counter++;                 /* free-running, wraps at 16 bits like the desk's */
	m->state_ticks++;

	enum reac_master_emit emit = REAC_M_EMIT_FILLER;
	int idx = 0;

	switch (m->state) {
	case REAC_M_IDLE:
		/* No box: keep emitting FILLER so the wire/clock stays alive (the box
		 * needs to see the downstream broadcast to begin its presence-flood). */
		emit = REAC_M_EMIT_FILLER;
		break;

	case REAC_M_PROBING:
		/* Hunting: interleave a cdea 01 probe (sub-state cycling) into the
		 * stream ~once per hb_period; the rest is FILLER. After probe_frames
		 * auto-advance to the grant burst (the real master grants on seeing the
		 * box's presence-flood; offline we time it). */
		if (++m->hb_tick >= m->hb_period) {
			m->hb_tick = 0;
			emit = REAC_M_EMIT_PROBE;
			idx = m->probe_sub_idx;          /* sub-state for THIS probe frame */
			m->probe_sub_idx = (m->probe_sub_idx + 1) & 3;
		}
		if (m->state_ticks >= m->probe_frames) {
			m->state = REAC_M_GRANTING;
			m->state_ticks = 0;
			m->hb_tick = 0;
		}
		break;

	case REAC_M_GRANTING:
		/* The ~150 ms connect-grant: a dense burst of cdea 04 03 (every frame in
		 * the window). When the burst ends, settle to ESTABLISHED. */
		emit = REAC_M_EMIT_GRANT;
		if (m->state_ticks >= m->grant_frames) {
			m->state = REAC_M_ESTABLISHED;
			m->state_ticks = 0;
			m->hb_tick = 0;
			m->chanmap_cursor = 0;
			m->announce_due = 0;
		}
		break;

	case REAC_M_ESTABLISHED:
		/* Linked: mostly FILLER audio, with one control frame per hb_period.
		 * Alternate the channel-map walk and the cfea announce on that slot so
		 * both stay ~1/s; walk the 6-frame chanmap cursor and never repeat a
		 * stale frame (the real master rotates the full 40-ch map). */
		if (++m->hb_tick >= m->hb_period) {
			m->hb_tick = 0;
			if (m->announce_due) {
				emit = REAC_M_EMIT_ANNOUNCE;
				m->announce_due = 0;
			} else {
				emit = REAC_M_EMIT_CHANMAP;
				idx = m->chanmap_cursor;     /* this frame's map slice */
				m->chanmap_cursor = (m->chanmap_cursor + 1) % REAC_M_CHANMAP_FRAMES;
				m->announce_due = 1;
			}
		}
		break;
	}

	if (tmpl_idx)
		*tmpl_idx = idx;
	return emit;
}

/* Overwrite type [16:18] + control block [18:50] of `frame` with `blk` (a 34-byte
 * [type|block] template), then re-apply the cdea/cfea checksum at [49]. The audio,
 * counter and C2/EA tail that reac_tx_build wrote are untouched. */
#define REAC_TYPE_OFF 16   /* type [16:18], control block [18:50] follows */

static void apply_block(uint8_t *frame, const uint8_t blk[34])
{
	memcpy(frame + REAC_TYPE_OFF, blk, 34); /* [16:50] = type[2] + block[32] */
	reac_ctrl_checksum_apply(frame);        /* re-stamp the checksum at [49]  */
}

int reac_master_stamp(uint8_t *frame, enum reac_master_emit emit, int chanmap_idx)
{
	switch (emit) {
	case REAC_M_EMIT_FILLER:
		/* reac_tx_build already wrote type 00 00 + zero block + audio + tail. */
		return 0;
	case REAC_M_EMIT_PROBE: {
		uint8_t blk[34];
		memcpy(blk, PROBE_BLK, 34);
		/* The probe sub-state lives at block byte [19] = blk[3]. The caller picks
		 * which sub-state via chanmap_idx (reused as the sub-state index 0..3). */
		int si = chanmap_idx & 3;
		blk[3] = REAC_M_PROBE_SUBSTATES[si];
		apply_block(frame, blk);
		return 0;
	}
	case REAC_M_EMIT_GRANT:
		apply_block(frame, GRANT_BLK);
		return 0;
	case REAC_M_EMIT_CHANMAP:
		if (chanmap_idx < 0 || chanmap_idx >= REAC_M_CHANMAP_FRAMES)
			return -1;
		apply_block(frame, CHANMAP[chanmap_idx]);
		return 0;
	case REAC_M_EMIT_ANNOUNCE:
		apply_block(frame, ANNOUNCE_BLK);
		return 0;
	}
	return -1;
}
