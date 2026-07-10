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
 *   ANNOUNCE_TMPL — the cfea master announce (inCh 0x28=40, outCh 0x10=16),
 *                  same capture. The capture embeds the desk's OWN MAC in the
 *                  payload; reac_master_init() rewrites that field to OUR src
 *                  (announce_blk) — advertising the cloned desk MAC while our
 *                  L2 src differs is a documented slave-disconnect trigger.
 *   PROBE_BLK    — cdea 01 SUB 001a (hunting), from §6. Byte [3] is the live
 *                  sub-state, cycled through REAC_M_PROBE_CYCLE (00-dominant)
 *                  with the checksum re-applied.
 *
 * There is NO canned grant block anymore: the golden transcript shows the
 * master ECHOES the box's own cdea 04 03 cold-connect back as the grant burst,
 * so REAC_M_EMIT_GRANT stamps m->join_blk verbatim.
 * TODO(rig): §13d hints the echoed grant alternates 0013/0e vs 0014/0f against
 * the box's block — not byte-verifiable offline (the /tmp pcaps are lost);
 * echo-verbatim is the closest grounded behaviour. Re-check on the next live
 * power-cycle capture (the JOIN/grant hex dumps in the pacer event log).
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

/* The embedded master-MAC field sits at template idx 11..16 (block [9:15]). */
#define ANNOUNCE_MAC_IDX 11
static const uint8_t ANNOUNCE_TMPL[34] = { 0xcf, 0xea, 0xff, 0xff, 0x01, 0x00, 0x01, 0x03, 0x0d, 0x01, 0x04, 0x00, 0x40, 0xab, 0xca, 0x15, 0x4d, 0x28, 0x10, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9a };
static const uint8_t PROBE_BLK[34]    = { 0xcd, 0xea, 0x01, 0x00, 0x00, 0x1a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc };

/* Probe sub-state cycle: 00 dominates (the §13d 120-250/s hunting band mostly
 * advertises ss=00), with 03/01/02 touched once per 8-probe loop. The earlier
 * 4-entry equal cycle advertised the linked ss=03 far too often. */
const uint8_t REAC_M_PROBE_CYCLE[8] = { 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };

const char *reac_master_state_name(enum reac_master_state s)
{
	switch (s) {
	case REAC_M_IDLE:        return "IDLE";
	case REAC_M_PROBING:     return "PROBING";
	case REAC_M_GRANTING:    return "GRANTING";
	case REAC_M_ESTABLISHED: return "ESTABLISHED";
	}
	return "?";
}

const char *reac_master_rx_event_name(enum reac_master_rx_event e)
{
	switch (e) {
	case REAC_M_RX_BOX_BCAST_FILLER: return "BCAST-FILLER";
	case REAC_M_RX_BOX_JOIN:         return "JOIN";
	case REAC_M_RX_BOX_UNICAST:      return "UNICAST";
	case REAC_M_RX_BOX_BYE:          return "BYE";
	}
	return "?";
}

const char *reac_master_drop_name(enum reac_master_drop_reason r)
{
	switch (r) {
	case REAC_M_DROP_NONE:          return "none";
	case REAC_M_DROP_PEER_GONE:     return "peer-gone";
	case REAC_M_DROP_BYE:           return "explicit-BYE";
	case REAC_M_DROP_MAC_CHANGE:    return "box-mac-change";
	case REAC_M_DROP_GRANT_TIMEOUT: return "grant-timeout";
	}
	return "?";
}

void reac_master_init(struct reac_master *m, const uint8_t src[6], int fps)
{
	memset(m, 0, sizeof *m);
	m->state = REAC_M_IDLE;
	memcpy(m->src, src, 6);
	m->fps = fps > 0 ? fps : 8000;

	/* ~180 probes/s (inside the transcribed 120-250/s hunting band). */
	m->probe_period = m->fps / 180;
	if (m->probe_period < 1)
		m->probe_period = 1;
	/* ~150 ms grant window, one echoed grant per stride (~100 frames total). */
	m->grant_frames = (m->fps * 15) / 100;
	if (m->grant_frames < 1)
		m->grant_frames = 1;
	m->grant_stride = REAC_M_GRANT_STRIDE;

	/* Per-instance cfea announce: captured template with OUR src MAC embedded
	 * (on-wire identity must match the L2 source), checksum recomputed. */
	memcpy(m->announce_blk, ANNOUNCE_TMPL, 34);
	memcpy(m->announce_blk + ANNOUNCE_MAC_IDX, m->src, 6);
	unsigned s = 0;
	for (int i = 2; i < 33; i++)   /* block bytes [18:49] = template [2:33] */
		s += m->announce_blk[i];
	m->announce_blk[33] = (uint8_t)((256 - (s & 0xff)) & 0xff);
}

/* Enter PROBING with a fresh probe/announce phase (from IDLE, a grant timeout,
 * or an established drop). The counter is NOT touched — it free-runs. */
static void enter_probing(struct reac_master *m)
{
	m->state = REAC_M_PROBING;
	m->probe_tick = 0;
	m->announce_tick = 0;
	m->probe_sub_idx = 0;
	/* Advertise the established channel-map while unlinked too (see the PROBING
	 * emit case). Phase-offset half a second from the cfea so the two 1 Hz
	 * control streams don't share a slot. */
	m->chanmap_tick = m->fps / 2;
	m->chanmap_cursor = 0;
}

/* Open a grant window echoing this JOIN block (also re-opens on a fresh JOIN
 * mid-window, and on a JOIN while established — the box restarted its
 * handshake, so we re-court it). */
static void enter_granting(struct reac_master *m, const uint8_t box_src[6],
                           const uint8_t blk32[32])
{
	m->state = REAC_M_GRANTING;
	m->grant_ticks = 0;
	m->grant_attempts++;
	memcpy(m->box_mac, box_src, 6);
	if (blk32)
		memcpy(m->join_blk, blk32, 32);
}

static void enter_established(struct reac_master *m)
{
	m->state = REAC_M_ESTABLISHED;
	m->chanmap_tick = 0;
	m->chanmap_cursor = 0;
	/* Phase-offset the cfea stream half a second from the chanmap stream so
	 * the two 1/s emissions never contend for the same slot. */
	m->announce_tick = m->fps / 2;
	m->link_check = REAC_M_LINKCHECK_RELOAD;
}

int reac_master_rx(struct reac_master *m, enum reac_master_rx_event ev,
                   const uint8_t box_src[6], const uint8_t blk32[32])
{
	/* Any box frame is presence (diagnostic only — never gates the grant). */
	m->box_seen = 1;
	m->presence_tick = REAC_M_PRESENCE_TIMEOUT;

	/* The pacer is ticking us if RX arrives; IDLE only means "first slot not
	 * emitted yet" — treat it as PROBING so an early JOIN is not lost. */
	if (m->state == REAC_M_IDLE)
		enter_probing(m);

	switch (m->state) {
	case REAC_M_IDLE:   /* unreachable (promoted above) */
	case REAC_M_PROBING:
		if (ev == REAC_M_RX_BOX_JOIN && blk32) {
			enter_granting(m, box_src, blk32);
			return 1;
		}
		/* Presence-flood / stray unicast: diagnostic only. Presence alone
		 * must NOT trigger the grant (the anti-#130 golden rule). */
		return 0;

	case REAC_M_GRANTING:
		if (ev == REAC_M_RX_BOX_JOIN && blk32) {
			/* A fresh JOIN restarts the window (the box retries on a ~100 ms
			 * grid); a JOIN from a different box re-latches to it. */
			enter_granting(m, box_src, blk32);
			return 0;
		}
		if (ev == REAC_M_RX_BOX_UNICAST) {
			/* "The instant the grant lands the box stops broadcasting and
			 * switches to unicast-to-master" — that switch IS the accept. */
			enter_established(m);
			return 1;
		}
		if (ev == REAC_M_RX_BOX_BYE) {
			m->drop_reason = REAC_M_DROP_BYE;
			enter_probing(m);
			return 1;
		}
		return 0;

	case REAC_M_ESTABLISHED:
		/* Every box RX event reloads the 600-frame link-check budget. */
		m->link_check = REAC_M_LINKCHECK_RELOAD;
		if (ev == REAC_M_RX_BOX_BYE) {
			m->drop_reason = REAC_M_DROP_BYE;
			enter_probing(m);
			return 1;
		}
		if (ev == REAC_M_RX_BOX_JOIN && blk32) {
			/* The box (or a different box) restarted its handshake. */
			if (memcmp(box_src, m->box_mac, 6) != 0)
				m->drop_reason = REAC_M_DROP_MAC_CHANGE;
			enter_granting(m, box_src, blk32);
			return 1;
		}
		return 0;
	}
	return 0;
}

enum reac_master_emit reac_master_next(struct reac_master *m, uint16_t *counter,
                                       int *tmpl_idx)
{
	*counter = m->counter;
	m->counter++;                 /* free-running, wraps at 16 bits like the desk's */

	/* First emitted slot: the pacer is running, so we probe — always. */
	if (m->state == REAC_M_IDLE)
		enter_probing(m);

	/* Presence diagnostic decay (logging only — never a state input). */
	if (m->box_seen && --m->presence_tick <= 0)
		m->box_seen = 0;

	enum reac_master_emit emit = REAC_M_EMIT_FILLER;
	int idx = 0;

	switch (m->state) {
	case REAC_M_IDLE:   /* unreachable (promoted above) */
	case REAC_M_PROBING:
		/* Hunting: FILLER by default; one cdea 01 probe per probe_period
		 * (~180/s, 00-heavy sub-state cycle) + one cfea announce per second
		 * (the real master announces @1 Hz even unlinked). One emission per
		 * slot: the announce takes priority, the probe fires the next slot
		 * (its tick is only reset when it actually emits). NO timer leaves
		 * this state — only a validated JOIN does (reac_master_rx). */
		m->announce_tick++;
		m->chanmap_tick++;
		m->probe_tick++;
		if (m->announce_tick >= m->fps) {
			m->announce_tick = 0;
			emit = REAC_M_EMIT_ANNOUNCE;
		} else if (m->chanmap_tick >= m->fps) {
			/* §4 (S-1608 firmware, FUN_0c003548): the box's establishment
			 * parser recognizes a master ONLY on the sub-state-0x03 established
			 * channel-map (cdea 01 03 0019 ...); a 00-dominant probe explicitly
			 * "keeps probing". A master that only probes and waits for the box's
			 * JOIN therefore deadlocks against a box that only joins once it has
			 * recognized a valid master. So advertise the chanmap walk while
			 * unlinked (the byte-exact captured CHANMAP frames) at 1 Hz — the box
			 * latches on the first valid 03 frame and initiates its cold-connect.
			 * The interleaved probes stay (a real master also cycles cdea 01). */
			m->chanmap_tick = 0;
			emit = REAC_M_EMIT_CHANMAP;
			idx = m->chanmap_cursor;
			m->chanmap_cursor = (m->chanmap_cursor + 1) % REAC_M_CHANMAP_FRAMES;
		} else if (m->probe_tick >= m->probe_period) {
			m->probe_tick = 0;
			emit = REAC_M_EMIT_PROBE;
			idx = m->probe_sub_idx;
			m->probe_sub_idx = (m->probe_sub_idx + 1) & 7;
		}
		break;

	case REAC_M_GRANTING:
		/* Echo the box's JOIN block back, one grant per grant_stride slots
		 * (~100 frames over the ~150 ms window — the transcribed real burst,
		 * NOT the old every-slot flood). Window expiry with no box unicast
		 * falls BACK to PROBING (never forward: the anti-regression rule);
		 * the box's ~100 ms JOIN retry grid self-heals a failed grant. */
		m->grant_ticks++;
		if ((m->grant_ticks - 1) % m->grant_stride == 0)
			emit = REAC_M_EMIT_GRANT;
		if (m->grant_ticks >= m->grant_frames) {
			m->drop_reason = REAC_M_DROP_GRANT_TIMEOUT;
			enter_probing(m);
		}
		break;

	case REAC_M_ESTABLISHED:
		/* Linked: TWO independent 1/s control streams — the chanmap walk and
		 * the cfea announce — phase-offset by half a second so they never
		 * collide on a slot (the old alternation halved both to 0.5 Hz).
		 * HOLD: the 600-frame budget counts down every slot; every box RX
		 * event reloads it (reac_master_rx). */
		m->chanmap_tick++;
		m->announce_tick++;
		if (m->chanmap_tick >= m->fps) {
			m->chanmap_tick = 0;
			emit = REAC_M_EMIT_CHANMAP;
			idx = m->chanmap_cursor;
			m->chanmap_cursor = (m->chanmap_cursor + 1) % REAC_M_CHANMAP_FRAMES;
		} else if (m->announce_tick >= m->fps) {
			m->announce_tick = 0;
			emit = REAC_M_EMIT_ANNOUNCE;
		}
		if (--m->link_check <= 0) {
			m->drop_reason = REAC_M_DROP_PEER_GONE;
			enter_probing(m);
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

int reac_master_stamp(const struct reac_master *m, uint8_t *frame,
                      enum reac_master_emit emit, int tmpl_idx)
{
	switch (emit) {
	case REAC_M_EMIT_FILLER:
		/* reac_tx_build already wrote type 00 00 + zero block + audio + tail. */
		return 0;
	case REAC_M_EMIT_PROBE: {
		uint8_t blk[34];
		memcpy(blk, PROBE_BLK, 34);
		/* The probe sub-state lives at block byte [19] = blk[3]; tmpl_idx is
		 * the cursor into the 00-dominant cycle. */
		blk[3] = REAC_M_PROBE_CYCLE[tmpl_idx & 7];
		apply_block(frame, blk);
		return 0;
	}
	case REAC_M_EMIT_GRANT: {
		/* The grant is the ECHO of the box's own cdea 04 03 block. */
		uint8_t blk[34];
		blk[0] = 0xcd; blk[1] = 0xea;
		memcpy(blk + 2, m->join_blk, 32);
		apply_block(frame, blk);
		return 0;
	}
	case REAC_M_EMIT_CHANMAP:
		if (tmpl_idx < 0 || tmpl_idx >= REAC_M_CHANMAP_FRAMES)
			return -1;
		apply_block(frame, CHANMAP[tmpl_idx]);
		return 0;
	case REAC_M_EMIT_ANNOUNCE:
		apply_block(frame, m->announce_blk);
		return 0;
	}
	return -1;
}
