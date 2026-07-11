// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_master.h"
#include "reac_ctrl.h"   /* reac_ctrl_checksum_apply, REAC_CTRL_* offsets */

#include <reac/reac.h>   /* REAC_FRAME_BYTES, REAC_END_MARKER_*, ... */
#include <string.h>

/* ------------------------------------------------------------------------- *
 * The downstream control plane: three FIXED protocol constants + two GENERATED
 * blocks (byte source-of-truth: reac-captures/m300-s1608-*.pcap, 2026-07-10,
 * real M-300 master 00:40:ab:c9:d8:5b driving an S-1608).
 *
 * A real master CONTINUOUSLY advertises five control messages; each is 34 bytes
 * [16:50] = the 2 type bytes (cd ea / cf ea) + the 32-byte control block
 * [18:50], with Sum(block[18..49]) mod 256 == 0.
 *
 *   PROBE / SUB01 / SUB02 — cdea 01 00 / 01 01 / 01 02. FIXED protocol
 *       constants replayed verbatim: the M-300 capture is the only source for
 *       them, so no config vs constant split is provable. (SUB01 carries an
 *       ASCII "1234" token at block bytes [7:11] that may be a per-unit id;
 *       for the S-1608 replay the box is bound to THIS master identity so the
 *       captured token is correct. Flagged in the task's openMitigations.)
 *   CHANMAP — cdea 01 03 0019: the 11-window SWEEP a real master advertises to
 *       tile the whole 40-slot fabric 0x00..0x2f (gen_chanmap), byte-faithful to
 *       the captured M-300 establish sweep. A box enrolls ONLY after it sees the
 *       window mapping ITS OWN slots, so the map is fabric-wide, not console-
 *       width (fixing #130: the old one-frame 0x00..0x06 map left every box mute).
 *   CFEA (announce) — cfea: GENERATED from the console cfg + OUR src MAC
 *       (gen_cfea): fixed head + our MAC + inCh 0x28(=40) + outCh + the console
 *       field, checksummed. On-wire identity must equal the L2 source, so the
 *       MAC is always OURS (never a cloned desk MAC — a slave-disconnect
 *       trigger). Fed the S-1608 cfg with the M-300 MAC it is byte-exact.
 *
 * There is NO canned grant block: the golden transcript shows the master
 * ECHOES the box's own cdea 04 03 cold-connect back as the grant burst, so
 * REAC_M_EMIT_GRANT stamps m->join_blk verbatim.
 * TODO(rig): §13d hints the echoed grant alternates 0013/0e vs 0014/0f against
 * the box's block — not byte-verifiable offline; echo-verbatim is the grounded
 * behaviour. Re-check on the next live power-cycle capture.
 * ------------------------------------------------------------------------- */

/* The three fixed M-300 control constants (byte-exact, checksum-valid). */
static const uint8_t PROBE_BLK[34] = { 0xcd, 0xea, 0x01, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xdd };
static const uint8_t SUB01_BLK[34] = { 0xcd, 0xea, 0x01, 0x01, 0x00, 0x18, 0x00, 0x22, 0xc8, 0x31, 0x32, 0x33, 0x34, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x80, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa7 };
static const uint8_t SUB02_BLK[34] = { 0xcd, 0xea, 0x01, 0x02, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7 };

/* The cfea master-MAC field sits at template idx 11..16 (block [9:15]). */
#define ANNOUNCE_MAC_IDX 11

/* Set blk[33] so the 32-byte control block [2:34] sums to 0 mod 256 (the
 * cdea/cfea checksum rule; identical to reac_ctrl_checksum_apply on a frame). */
static void stamp_block_cksum(uint8_t blk[34])
{
	unsigned s = 0;
	for (int i = 2; i < 33; i++)   /* block bytes [18:49] = template [2:33] */
		s += blk[i];
	blk[33] = (uint8_t)((256 - (s & 0xff)) & 0xff);
}

/* Generate the cfea master-announce from the console cfg + OUR src MAC.
 *
 * cfea width byte (out[18], the byte AFTER the fixed 0x28=40-slot total): CORRECTED
 * 2026-07-11 by a two-model M-200 compare (S-1608 vs S-0808 on desk c9:cc:03) — it
 * is the CONNECTED BOX's INPUT width, NOT the box output count: idle default 0x08,
 * 0x10 once a 16-in S-1608 links, 0x08 once an 8-in S-0808 links, with a box-count
 * field going 0x0000 -> 0x0001 (out[20:22]). Both boxes are 8-OUT yet the byte
 * differs, so it tracks INPUT; the chanmap (which follows OUTPUT width) is identical
 * for both. We currently emit the STATIC idle-form (out[18]=cfg->out_channels, which
 * is 0x08 for the S-1608 default so it matches the idle capture byte-for-byte) — a
 * known master-role fidelity gap: the live master should raise this to the box's
 * input width + set the box-count on sync. Unchanged here: it does not affect the
 * slave-side #130 establishment fix, and the idle bytes stay M-300-exact. */
static void gen_cfea(uint8_t out[34], const uint8_t src[6],
                     const struct reac_console_cfg *cfg)
{
	static const uint8_t head[11] =
		{ 0xcf, 0xea, 0xff, 0xff, 0x01, 0x00, 0x01, 0x03, 0x0d, 0x01, 0x04 };
	memset(out, 0, 34);
	memcpy(out, head, 11);
	memcpy(out + ANNOUNCE_MAC_IDX, src, 6);   /* OUR MAC = the L2 source */
	out[17] = 0x28;                 /* 40: the FIXED REAC downstream slot total   */
	out[18] = cfg->out_channels;    /* box width byte (idle-form; see note above) */
	out[19] = cfg->console_field;   /* console field (M-300 = 0, M-5000 = 1)      */
	out[20] = 0x00;                 /* box-count hi (idle 0; ->0x0001 on sync)    */
	out[21] = cfg->console_field;   /* moves in lockstep with [19]                */
	/* [22:33] stay zero */
	stamp_block_cksum(out);
}

/* The channel-map SWEEP a real master advertises. GROUND TRUTH: the captured
 * M-300 establish sweep (reac-captures/m300-s1608-establish-2026-07-10.pcap, real
 * M-300 00:40:ab:c9:d8:5b driving an S-1608) emits ELEVEN distinct cdea 01 03 0019
 * frames — sliding 8-slot windows that together tile the whole 40-slot REAC fabric
 * 0x00..0x2f — in the fixed rotation { fe, 07, 0f, 17, 1f, 27, 2f, 06, 0e, 16, 1e }.
 *
 * WHY THE SWEEP (not one frame): §4 (S-1608 firmware FUN_0c003548) — a box
 * recognizes a master ONLY once it has seen the window that maps ITS OWN slots. An
 * S-0808 owns 0x00..0x07, an S-1608 0x00..0x0f; NEITHER is covered by a single
 * 0x00..0x06 window. The earlier one-frame map (derived from the local console's
 * out_channels) advertised only 0x00..0x06, so every real box stayed silent — the
 * root cause of #130, pinned by an offline field-diff of our downstream vs
 * m300-s1608-establish / m200-probing-nobox (2026-07-11). The master advertises the
 * FABRIC, not the console's output count, so the sweep is fixed and cfg-independent.
 *
 * Each window is 8 slots. A slot is either the 0xfe section marker (-> fe 00 00,
 * sitting where the fabric ring wraps: windows 2f and fe) or a channel id ch with
 * value 0x28 (ch <= 0x27) / 0x38 (0x28 <= ch <= 0x2f). apply_block re-checksums at
 * emit time, so only the slot bytes matter; stamp_block_cksum keeps the stored
 * template self-consistent too. This generator reproduces all 11 captured blocks
 * byte-for-byte incl. their checksums (asserted in tests/test_reac_s1608.c). */
#define REAC_CHANMAP_SWEEP_FRAMES 11
#define REAC_CHANMAP_MARKER 0xfe
static const uint8_t CHANMAP_SWEEP[REAC_CHANMAP_SWEEP_FRAMES][8] = {
	{ 0xfe, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 },
	{ 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e },
	{ 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 },
	{ 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e },
	{ 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 },
	{ 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e },
	{ 0x2f, 0xfe, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 },
	{ 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d },
	{ 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 },
	{ 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d },
	{ 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 },
};

/* Populate `frames` with the fixed 11-window fabric sweep (returns the count).
 * cfg is unused: the master advertises the whole fabric regardless of console
 * width (see the block comment above). */
static int gen_chanmap(uint8_t frames[][34], const struct reac_console_cfg *cfg)
{
	(void)cfg;
	for (int f = 0; f < REAC_CHANMAP_SWEEP_FRAMES; f++) {
		uint8_t *blk = frames[f];
		memset(blk, 0, 34);
		blk[0] = 0xcd; blk[1] = 0xea;
		blk[2] = 0x01; blk[3] = 0x03;         /* established sub-state 0x03 */
		blk[4] = 0x00; blk[5] = 0x19;         /* BE len 0x0019 (fixed)      */
		blk[6] = 0x01;                        /* payload-type = 1           */
		for (int s = 0; s < 8; s++) {
			uint8_t ch = CHANMAP_SWEEP[f][s];
			uint8_t *t = blk + 7 + s * 3;     /* 3-byte slot */
			if (ch == REAC_CHANMAP_MARKER) {
				t[0] = 0xfe; t[1] = 0x00; t[2] = 0x00;  /* section marker */
			} else {
				t[0] = ch;
				/* 0x38 for the high bank (0x28..0x2f), else 0x28. */
				t[1] = (ch >= 0x28 && ch <= 0x2f) ? 0x38 : 0x28;
				t[2] = 0x00;
			}
		}
		blk[31] = 0x00; blk[32] = 0x00;       /* terminator */
		stamp_block_cksum(blk);
	}
	return REAC_CHANMAP_SWEEP_FRAMES;
}

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

void reac_master_init(struct reac_master *m, const uint8_t src[6],
                      const struct reac_console_cfg *cfg, int fps)
{
	memset(m, 0, sizeof *m);
	m->state = REAC_M_IDLE;
	memcpy(m->src, src, 6);
	m->fps = fps > 0 ? fps : 8000;
	m->cfg = cfg ? *cfg : REAC_CONSOLE_CFG_S1608;

	/* ~115 probes/s (the M-300's measured probe rate). */
	m->probe_period = m->fps / 115;
	if (m->probe_period < 1)
		m->probe_period = 1;
	/* ~150 ms grant window, one echoed grant per stride (~100 frames total). */
	m->grant_frames = (m->fps * 15) / 100;
	if (m->grant_frames < 1)
		m->grant_frames = 1;
	m->grant_stride = REAC_M_GRANT_STRIDE;

	/* Generate the downstream the master advertises for this console: the
	 * chanmap frame(s) + the cfea announce (OUR src MAC embedded). */
	m->chanmap_nframes = gen_chanmap(m->chanmap, &m->cfg);
	gen_cfea(m->announce_blk, m->src, &m->cfg);
}

/* Phase-offset the four 1/s control streams (sub01/sub02/chanmap/cfea) by fps/4
 * each so no two ever fall due on the same slot. A tick started at offset X
 * fires after (fps - X) slots. */
static void reset_control_cadence(struct reac_master *m)
{
	m->probe_tick    = 0;
	m->sub01_tick    = 0;
	m->sub02_tick    = m->fps / 4;
	m->chanmap_tick  = m->fps / 2;
	m->announce_tick = (3 * m->fps) / 4;
	m->chanmap_cursor = 0;
}

/* Enter PROBING with a fresh control cadence (from IDLE, a grant timeout, or an
 * established drop). The counter is NOT touched — it free-runs. */
static void enter_probing(struct reac_master *m)
{
	m->state = REAC_M_PROBING;
	reset_control_cadence(m);
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
	/* Same continuous control cadence as PROBING (PROBE + the four 1/s
	 * streams), phase-offset so they never contend for a slot. */
	reset_control_cadence(m);
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

/* The continuous 5-message control cadence a real master advertises in BOTH the
 * unlinked and the linked state (byte-exact against the M-300/S-1608 capture):
 * PROBE ~115/s + sub01/sub02/chanmap/cfea @1/s each. At most ONE control block
 * is emitted per slot (the rest carry audio FILLER). The four 1/s streams are
 * phase-offset by fps/4 (reset_control_cadence) so they never fall due together;
 * they take priority over the dense probe, whose tick only resets when it
 * actually fires so a preempted probe emits the next free slot.
 *
 * §4 (S-1608 firmware, FUN_0c003548): the box's parser recognizes a master ONLY
 * on the sub-state-0x03 channel-map (cdea 01 03 0019 ...), so the chanmap must
 * be advertised while unlinked too — a master that only probes deadlocks against
 * a box that only joins once it has seen a valid map. */
static enum reac_master_emit control_cadence(struct reac_master *m, int *idx)
{
	*idx = 0;
	m->probe_tick++;
	m->sub01_tick++;
	m->sub02_tick++;
	m->chanmap_tick++;
	m->announce_tick++;

	if (m->sub01_tick >= m->fps) {
		m->sub01_tick = 0;
		return REAC_M_EMIT_SUB01;
	}
	if (m->sub02_tick >= m->fps) {
		m->sub02_tick = 0;
		return REAC_M_EMIT_SUB02;
	}
	if (m->chanmap_tick >= m->fps) {
		m->chanmap_tick = 0;
		*idx = m->chanmap_cursor;
		m->chanmap_cursor = (m->chanmap_cursor + 1) % m->chanmap_nframes;
		return REAC_M_EMIT_CHANMAP;
	}
	if (m->announce_tick >= m->fps) {
		m->announce_tick = 0;
		return REAC_M_EMIT_ANNOUNCE;
	}
	if (m->probe_tick >= m->probe_period) {
		m->probe_tick = 0;
		return REAC_M_EMIT_PROBE;
	}
	return REAC_M_EMIT_FILLER;
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
		/* Unlinked: the full continuous control cadence. NO timer leaves this
		 * state — only a validated JOIN does (reac_master_rx). */
		emit = control_cadence(m, &idx);
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
		/* Linked: the SAME continuous control cadence as PROBING. HOLD: the
		 * 600-frame budget counts down every slot; every box RX event reloads
		 * it (reac_master_rx). */
		emit = control_cadence(m, &idx);
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

/* The downstream FILLER control-block descriptor (#130 fix 2). A real master
 * does NOT leave [18:50] all-zero on a FILLER frame: it repeats one non-zero
 * 16-bit value 16x across the block. Empirically confirmed 2026-07-10 against
 * reac-captures/m{200,300}-s1608-establish-2026-07-10.pcap (two different real
 * consoles, >1.1M FILLER frames total, offline pcap analysis, no rig): every
 * single captured FILLER block is exactly 16 copies of a 2-byte pair "00 xx"
 * (block[2k]=0x00 constant, block[2k+1]=the varying byte, k=0..15) — ZERO
 * all-zero blocks bar a literal handful (34 of 1.1M) at value-transition
 * boundaries, certainly an FPGA register-read race.
 *
 * The value is NOT a fixed per-console protocol constant — it is LIVE: a slow
 * ~1 Hz scan through ~15-20 widely spaced values before the box locks, then a
 * tight +/-1..3 dither around a per-session baseline (0xd9-0xe7 observed on
 * BOTH the M-200 and the M-300 captures) once running steadily. No
 * correlation was found against our free-running counter (every
 * `(counter >> k) & 0xff`, k=0..8, tested) or wall-clock time
 * (`elapsed_ms % 256`, tested) — under 4% match rate for either, chance
 * level. The best-supported read is a live analog telemetry sample (a
 * PLL-jitter or ADC noise-floor readback, maybe a periodic channel/meter
 * scan), not a protocol field — so it is UNLIKELY to be handshake-load-
 * bearing, but that is not proven offline.
 *
 * We reproduce the STRUCTURE exactly (16x one non-zero byte, high byte 0x00)
 * with a FIXED value drawn from the steady-state cluster shared by both
 * captured consoles. This is a plausible-pattern fix, not a byte-exact one:
 * matching the real live value is not offline-derivable. The rig test will
 * confirm whether a real box cares. FILLER stays checksum-exempt (this never
 * touches the checksum byte's semantics — [49] here is just descriptor data,
 * not a checksum). */
#define FILLER_DESC_BYTE 0xdc   /* one observed steady-state sample, both M-200 + M-300 */

static void stamp_filler_descriptor(uint8_t *frame)
{
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_BLOCK_END; i += 2) {
		frame[i] = 0x00;
		frame[i + 1] = FILLER_DESC_BYTE;
	}
}

int reac_master_stamp(const struct reac_master *m, uint8_t *frame,
                      enum reac_master_emit emit, int tmpl_idx)
{
	switch (emit) {
	case REAC_M_EMIT_FILLER:
		/* reac_tx_build (or the pacer's silent-underrun filler) already wrote
		 * type 00 00 + audio + tail; stamp the non-zero descriptor pattern a
		 * real master repeats there on EVERY FILLER frame (#130 fix 2). */
		stamp_filler_descriptor(frame);
		return 0;
	case REAC_M_EMIT_PROBE:
		apply_block(frame, PROBE_BLK);   /* the fixed M-300 probe */
		return 0;
	case REAC_M_EMIT_SUB01:
		apply_block(frame, SUB01_BLK);   /* the fixed M-300 cdea 01 01 */
		return 0;
	case REAC_M_EMIT_SUB02:
		apply_block(frame, SUB02_BLK);   /* the fixed M-300 cdea 01 02 */
		return 0;
	case REAC_M_EMIT_GRANT: {
		/* The grant is the ECHO of the box's own cdea 04 03 block. */
		uint8_t blk[34];
		blk[0] = 0xcd; blk[1] = 0xea;
		memcpy(blk + 2, m->join_blk, 32);
		apply_block(frame, blk);
		return 0;
	}
	case REAC_M_EMIT_CHANMAP:
		if (tmpl_idx < 0 || tmpl_idx >= m->chanmap_nframes)
			return -1;
		apply_block(frame, m->chanmap[tmpl_idx]);
		return 0;
	case REAC_M_EMIT_ANNOUNCE:
		apply_block(frame, m->announce_blk);
		return 0;
	}
	return -1;
}
