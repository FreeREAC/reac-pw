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

/* The two fixed M-300 control constants (byte-exact, checksum-valid). The PROBE is
 * NOT a constant — it rotates; see gen_probe(). */
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

/* ---- PROBE: the rotating hunt sequence (#130) -----------------------------
 * Measured live off an M-200 driving an S-1608 (2026-07-11): the cdea 01 00 001a
 * probe's 27-byte payload is a sliding window over the period-10 sequence
 *   [ 00 00 00 01 00 00 00 00 00 SUB ]
 * with SUB = 0x02 while hunting, 0x03 once established. The phase advances +6
 * (mod 10) after every 2 emissions, yielding the observed 0,6,2,8,4 rotation.
 * All 10 rotating probe blocks in the capture reproduce exactly under this model.
 * Our old code replayed ONE frozen phase (6/0x02, checksum 0xdd) forever. */
#define REAC_PROBE_PERIOD     10
#define REAC_PROBE_PHASE_STEP  6   /* phase += 6 (mod 10) -> 0,6,2,8,4 */
#define REAC_PROBE_REPEAT      2   /* emissions per phase before advancing */
#define REAC_PROBE_PAYLOAD    27   /* block[4:31] */

static void gen_probe(uint8_t blk[34], int phase, uint8_t sub)
{
	const uint8_t per[REAC_PROBE_PERIOD] = { 0, 0, 0, 1, 0, 0, 0, 0, 0, sub };
	memset(blk, 0, 34);
	blk[0] = 0xcd; blk[1] = 0xea;
	blk[2] = 0x01; blk[3] = 0x00;   /* cdea 01 00      */
	blk[4] = 0x00; blk[5] = 0x1a;   /* BE len 0x001a   */
	for (int i = 0; i < REAC_PROBE_PAYLOAD; i++)
		blk[6 + i] = per[(phase + i) % REAC_PROBE_PERIOD];
	stamp_block_cksum(blk);         /* -> blk[33] */
}

/* The 4 INVENTORY SPECIALS (#130): every probe burst carries, at in-burst probe
 * indices 30..33, four one-off probe variants — measured on the M-300/S-1608
 * establish capture at exactly those indices in EVERY burst (11/11), and present
 * in the M-200 corpora too. Bytes verbatim from the M-300 (checksum-valid); the
 * MAC special embeds the master's OWN MAC at block[7:13] (template [9:15]) — we
 * substitute OURS and re-checksum. A master that never sends these is another
 * tell of a dead downstream. */
#define REAC_PROBE_SPECIAL_FIRST 30
#define REAC_PROBE_SPECIAL_COUNT  4
#define PROBE_SPECIAL_MAC_IDX     9   /* template idx of the 6-byte MAC */
static const uint8_t PROBE_SPECIALS[REAC_PROBE_SPECIAL_COUNT][34] = {
	/* zeros-tail variant (cksum dd) */
	{ 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,
	  0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xdd },
	/* our-MAC variant (M-300 bytes carried c9:d8:5b at [9:15]; ours substituted) */
	{ 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0x00,0x00,0x00,0x40,0xab,0xc9,0xd8,0x5b,0x00,
	  0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x08 },
	/* "SYSP" variant */
	{ 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x53,0x59,0x53,0x50,0x01,0x00,0x00,0x00,0x00,0x97 },
	/* "SCEN" variant */
	{ 0xcd,0xea,0x01,0x00,0x00,0x1a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	  0x00,0x53,0x43,0x45,0x4e,0x01,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xb7 },
};

/* Build the probe for the CURRENT burst position + link state, publish its
 * checksum as the FILLER descriptor (every FILLER until the next probe carries
 * it — byte-verified against the M-200: P:de -> F:de x16 -> P:dd -> F:dd x16 ...),
 * then advance the rotation. m->probe_idx (set by the cadence) selects the 4
 * inventory specials at in-burst indices 30..33; all other indices emit the
 * rotating hunt probe. Called when the cadence decides to emit a PROBE, BEFORE
 * reac_master_stamp reads m->probe_blk. */
static void probe_prepare(struct reac_master *m)
{
	int sp = m->probe_idx - REAC_PROBE_SPECIAL_FIRST;
	if (sp >= 0 && sp < REAC_PROBE_SPECIAL_COUNT) {
		memcpy(m->probe_blk, PROBE_SPECIALS[sp], 34);
		if (sp == 1) {   /* the MAC special advertises OUR identity */
			memcpy(m->probe_blk + PROBE_SPECIAL_MAC_IDX, m->src, 6);
			stamp_block_cksum(m->probe_blk);
		}
		m->filler_desc = m->probe_blk[33];
		return;          /* the rotation is not advanced by a special */
	}

	uint8_t sub = (m->state == REAC_M_ESTABLISHED) ? 0x03 : 0x02;
	gen_probe(m->probe_blk, m->probe_phase, sub);
	m->filler_desc = m->probe_blk[33];

	if (++m->probe_repeat >= REAC_PROBE_REPEAT) {
		m->probe_repeat = 0;
		m->probe_phase = (m->probe_phase + REAC_PROBE_PHASE_STEP) % REAC_PROBE_PERIOD;
	}
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

/* The channel-map SWEEP a real master advertises. GROUND TRUTH: a LIVE M-200
 * (00:40:ab:c9:cc:03) driving a real S-1608 to sync, captured 2026-07-11 on the
 * rig. It emits FORTY-NINE distinct cdea 01 03 0019 windows — one per fabric ring
 * position — cycling continuously.
 *
 * THE FABRIC IS A RING of 49 positions: channels 0x00..0x2f (48) then the 0xfe
 * section marker at the wrap. A window advertises 8 CONSECUTIVE ring positions, so
 * windows near the wrap run through the marker and back to 0x00 (e.g. start 0x2f ->
 * 2f fe 00 01 02 03 04 05). One window per start position => exactly 49.
 *
 * WHY THE FULL SWEEP: §4 (S-1608 firmware FUN_0c003548) — a box recognizes a master
 * ONLY once it has seen the window that maps ITS OWN slots. The original code
 * derived the map from the local console's out_channels and emitted a SINGLE
 * 0x00..0x06 window, so no real box ever saw its channels and every one stayed
 * mute (#130). A first fix emitted 11 windows — but that figure came from an M-300
 * capture too SHORT to hold the whole rotation; the live M-200 shows the true 49.
 * The master advertises the FABRIC, never the console width: cfg is unused.
 *
 * Slot encoding: the 0xfe marker -> (fe 00 00); a channel ch -> (ch, val, 00) with
 * val 0x28 for ch <= 0x27 and 0x38 for the high bank 0x28..0x2f. apply_block
 * re-checksums at emit time, so only the slot bytes matter here; stamp_block_cksum
 * keeps the stored template self-consistent too. This generator reproduces the
 * captured windows byte-for-byte incl. checksums (tests/test_reac_s1608.c). */
#define REAC_CHANMAP_MARKER 0xfe
#define REAC_CHANMAP_SLOTS   8   /* ring positions advertised per frame */

/* The fabric RING: 49 positions — channels 0x00..0x2f (ring idx 0..47) then the
 * 0xfe section marker at the wrap (idx 48). */
static uint8_t ring_at(int i)
{
	i %= REAC_M_FABRIC_RING;
	return (i == REAC_M_FABRIC_RING - 1) ? REAC_CHANMAP_MARKER : (uint8_t)i;
}

/* The master's window emit ORDER (measured off the M-200): the marker window
 * first, then base 7 down to 0, each base stepping by 8 —
 *   fe · 07 0f 17 1f 27 2f · 06 0e 16 1e 26 2e · … · 00 08 10 18 20 28
 * i.e. 1 + 8*6 = 49 windows. Returns the ring START index of frame f. */
static int chanmap_start(int f)
{
	if (f == 0)
		return REAC_M_FABRIC_RING - 1;      /* the 0xfe marker window */
	int i    = f - 1;                       /* 0..47 */
	int base = 7 - (i / 6);                 /* 7,6,5,4,3,2,1,0 */
	int k    = i % 6;                       /* 0..5 */
	return base + 8 * k;
}

/* Populate `frames` with the full 49-window fabric sweep (returns the count).
 * cfg is unused: the master advertises the whole FABRIC, never the console's own
 * width (see the block comment above). */
static int gen_chanmap(uint8_t frames[][34], const struct reac_console_cfg *cfg)
{
	(void)cfg;
	for (int f = 0; f < REAC_M_FABRIC_RING; f++) {
		uint8_t *blk = frames[f];
		memset(blk, 0, 34);
		blk[0] = 0xcd; blk[1] = 0xea;
		blk[2] = 0x01; blk[3] = 0x03;         /* established sub-state 0x03 */
		blk[4] = 0x00; blk[5] = 0x19;         /* BE len 0x0019 (fixed)      */
		blk[6] = 0x01;                        /* payload-type = 1           */
		int start = chanmap_start(f);
		for (int s = 0; s < REAC_CHANMAP_SLOTS; s++) {
			uint8_t ch = ring_at(start + s);
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
	return REAC_M_FABRIC_RING;
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

	/* The cycle-locked control choreography, all slot offsets measured on the
	 * M-300/S-1608 establish capture at 4000 fps and scaled by fps (see the
	 * struct doc): cycle 10778 slots; probes every 8 slots through slot 2720
	 * (341/burst incl. the 4 specials at indices 30..33); sub02 at 2728;
	 * chanmap at 5953; sub01 at 10773 (cycle_len - 5). */
	m->cycle_len    = (int)(((int64_t)m->fps * 10778) / 4000);
	m->probe_stride = m->fps / 500;
	if (m->probe_stride < 1)
		m->probe_stride = 1;
	m->burst_end    = (341 - 1) * m->probe_stride;
	m->sub02_off    = m->burst_end + m->probe_stride;
	m->chanmap_off  = (int)(((int64_t)m->fps * 5953) / 4000);
	m->sub01_off    = m->cycle_len - 5;
	/* ~150 ms grant window, one echoed grant per stride (~100 frames total). */
	m->grant_frames = (m->fps * 15) / 100;
	if (m->grant_frames < 1)
		m->grant_frames = 1;
	m->grant_stride = REAC_M_GRANT_STRIDE;
	/* Peer-gone budget = ~6.5 s of frames (the measured M-200i hold), rate-scaled
	 * so it is the same wall-clock at 44.1/48/96k (#130). */
	m->link_check_reload = (m->fps * REAC_M_LINKCHECK_SECONDS_X10) / 10;
	if (m->link_check_reload < 1)
		m->link_check_reload = 1;

	/* Generate the downstream the master advertises for this console: the 49-window
	 * fabric sweep + the cfea announce (OUR src MAC embedded). */
	m->chanmap_nframes = gen_chanmap(m->chanmap, &m->cfg);
	gen_cfea(m->announce_blk, m->src, &m->cfg);

	/* Seed the probe rotation at phase 0 / sub 0x02 (hunting) so FILLER frames
	 * carry a valid descriptor from the very first slot, before any probe fires. */
	m->probe_phase  = 0;
	m->probe_repeat = 0;
	gen_probe(m->probe_blk, m->probe_phase, 0x02);
	m->filler_desc = m->probe_blk[33];
}

/* Restart the control cycle at slot 0 (the burst head — the first slot emits a
 * probe, exactly like a real master opening a hunt burst). cfea free-runs on its
 * own ~1/s tick, phase-offset so it lands in the pause region, never on a burst
 * probe slot. */
static void reset_control_cadence(struct reac_master *m)
{
	m->cycle_pos      = 0;
	m->announce_tick  = (3 * m->fps) / 4;
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
	m->link_check = m->link_check_reload;
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
		m->link_check = m->link_check_reload;
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

/* The CYCLE-LOCKED control cadence a real master advertises in BOTH the unlinked
 * and the linked state, measured slot-exact on the M-300/S-1608 establish
 * capture (#130): one deterministic cycle of cycle_len slots holding a probe
 * BURST (one probe every probe_stride slots through burst_end — 341 probes with
 * the 4 inventory specials at in-burst indices 30..33), then a probe-free pause
 * carrying sub02 (right after the burst), ONE chanmap window (mid-pause; the
 * 49-window sweep spans 49 cycles) and sub01 (cycle tail). cfea free-runs at
 * ~1/s and defers by a slot when it collides with a cycle event — the real
 * desk's cfea lands between burst probes too.
 *
 * The old model (PROBE ~115/s uniform + all four streams at 1/s) was the DUTY-
 * CYCLE AVERAGE of this rhythm — an analysis artifact a real box never sees on
 * the wire, and plausibly the last tell that kept real boxes mute (#130).
 *
 * §4 (S-1608 firmware, FUN_0c003548): the box's parser recognizes a master ONLY
 * on the sub-state-0x03 channel-map (cdea 01 03 0019 ...), so the chanmap must
 * be advertised while unlinked too — a master that only probes deadlocks against
 * a box that only joins once it has seen a valid map. */
static enum reac_master_emit control_cadence(struct reac_master *m, int *idx)
{
	*idx = 0;
	int pos = m->cycle_pos;
	m->cycle_pos = (pos + 1) % m->cycle_len;
	m->announce_tick++;

	/* PROBE burst only while HUNTING. A real M-200i emits ZERO 0100001a probes
	 * once ESTABLISHED (measured 2026-07-11: 0 probes across the whole linked
	 * window, only chanmap + audio); the probe is the hunt beacon, so suppress the
	 * burst once locked — those slots fall through to audio FILLER. (TODO: the
	 * linked-state chanmap runs ~2/s on the real desk vs our 1/cycle — a fidelity
	 * refinement, not establishment-critical since the box is already locked.) */
	if (m->state != REAC_M_ESTABLISHED &&
	    pos <= m->burst_end && pos % m->probe_stride == 0) {
		m->probe_idx = pos / m->probe_stride;  /* specials key off this */
		return REAC_M_EMIT_PROBE;
	}
	if (pos == m->sub02_off)
		return REAC_M_EMIT_SUB02;
	if (pos == m->chanmap_off) {
		*idx = m->chanmap_cursor;
		m->chanmap_cursor = (m->chanmap_cursor + 1) % m->chanmap_nframes;
		return REAC_M_EMIT_CHANMAP;
	}
	if (pos == m->sub01_off)
		return REAC_M_EMIT_SUB01;
	if (m->announce_tick >= m->fps) {
		m->announce_tick = 0;
		return REAC_M_EMIT_ANNOUNCE;
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

	/* Build this slot's probe (current phase + link-state sub) and publish its
	 * checksum as the FILLER descriptor, then advance the rotation. Must run
	 * BEFORE reac_master_stamp reads m->probe_blk / m->filler_desc. */
	if (emit == REAC_M_EMIT_PROBE)
		probe_prepare(m);

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

/* The downstream FILLER control-block descriptor. A real master never leaves
 * [18:50] all-zero on a FILLER frame: it repeats the 2-byte pair "00 xx" 16x.
 *
 * SOLVED 2026-07-11 (live M-200 + S-1608 on the rig, #130). `xx` is NOT telemetry
 * and NOT a per-console constant — it is the CHECKSUM OF THE CURRENT PROBE. The
 * probe rotates (gen_probe), and every FILLER emitted until the next probe carries
 * that probe's checksum, byte-verified on the wire:
 *
 *   P:de P:de  F:de x16   P:dd P:dd  F:dd x16   P:dc P:dc  F:dc x10  ...
 *
 * An earlier RE pass mistook the resulting cycle for "a live analog telemetry
 * sample (PLL-jitter / ADC noise-floor readback), UNLIKELY to be handshake-load-
 * bearing" — that reading was WRONG, and it hid the real defect: because our probe
 * was frozen, our descriptor was frozen too (a constant 0xdc), so our downstream
 * never presented the rotating hunt state a box expects.
 *
 * FILLER stays checksum-exempt: [49] here is descriptor data, not a checksum. */
static void stamp_filler_descriptor(uint8_t *frame, uint8_t desc)
{
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_BLOCK_END; i += 2) {
		frame[i] = 0x00;
		frame[i + 1] = desc;
	}
}

int reac_master_stamp(const struct reac_master *m, uint8_t *frame,
                      enum reac_master_emit emit, int tmpl_idx)
{
	switch (emit) {
	case REAC_M_EMIT_FILLER:
		/* reac_tx_build (or the pacer's silent-underrun filler) already wrote
		 * type 00 00 + audio + tail; stamp 16x "00 <current-probe-checksum>",
		 * which is exactly what a real master repeats there (#130). */
		stamp_filler_descriptor(frame, m->filler_desc);
		return 0;
	case REAC_M_EMIT_PROBE:
		apply_block(frame, m->probe_blk); /* the ROTATING probe (probe_prepare) */
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
