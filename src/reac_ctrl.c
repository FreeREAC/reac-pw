// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_ctrl.h"
#include <reac/reac.h>
#include <reac/reac_encode.h>  /* reac_braid_encode — the layout oracle's encode side */
#include <string.h>
#include <math.h>

/* Frame geometry (ground-truthed against reac-captures/wired-reac-a-bothdirs):
 *   master->fabric frames: 1492 B (40ch width), broadcast, trailer C2 EA.
 *   box->master frames:    18 hdr + 32 descriptor + n_ch*36 audio + 2 tail.
 *     16ch -> 628 B, 8ch -> 340 B. The [18:50] block is the descriptor on a
 *     FILLER (00 7a per slot) or the cdea/cfea control on a control frame; the
 *     audio region [50:..] still carries 12 samples/ch either way. */
#define ETH_HDR 14
#define CNT_OFF 14
#define TYPE_OFF 16
#define AUDIO_OFF 50
#define DESC_WORD_HI 0x00
#define DESC_WORD_LO 0x7a   /* per-channel descriptor byte observed on the wire */

static inline void put_hdr(uint8_t *f, const uint8_t dst[6], const uint8_t src[6],
                           uint16_t counter, uint8_t t0, uint8_t t1)
{
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[CNT_OFF] = (uint8_t)(counter & 0xff);
	f[CNT_OFF + 1] = (uint8_t)(counter >> 8);
	f[TYPE_OFF] = t0; f[TYPE_OFF + 1] = t1;
}

int reac_ctrl_classify_box_frame(const uint8_t *frame, size_t len,
                                 const uint8_t our_mac[6],
                                 struct reac_ctrl_parsed *out,
                                 enum reac_master_rx_event *ev)
{
	if (reac_ctrl_parse(frame, len, out) == REAC_CTRL_NONE)
		return -1;                                   /* not a 0x8819 frame */
	/* Roland OUI only, and never our own echo (mandatory belt-and-braces:
	 * PACKET_IGNORE_OUTGOING is best-effort and hubs/loopbacks echo). */
	if (out->src[0] != 0x00 || out->src[1] != 0x40 || out->src[2] != 0xab)
		return -1;
	if (memcmp(out->src, our_mac, 6) == 0)
		return -1;

	const int to_us = (memcmp(out->dst, our_mac, 6) == 0);

	/* A cdea/cfea control frame with an invalid checksum is corrupt — never a
	 * JOIN, never a heartbeat, never evidence of anything. FILLER (type 0000)
	 * is checksum-exempt (the block is the audio descriptor). */
	if (out->kind != REAC_CTRL_FILLER && reac_ctrl_checksum_verify(frame) != 0)
		return -1;

	/* The box cold-connect JOIN: cdea 04 03, BE len 0x13/0x14, then 00 02.
	 * Keyed ONLY on block[0:6] + checksum — the tail is device inventory
	 * (0x41 is NOT a MAC tail). Broadcast AND unicast accepted (the box emits
	 * it x3 on PHY-up while still in broadcast mode). */
	if (out->kind == REAC_CTRL_GRANT) {
		if ((out->op_len == 0x0013 || out->op_len == 0x0014) &&
		    out->sel == 0x00 && out->sel2 == 0x02) {
			*ev = REAC_M_RX_BOX_JOIN;
			return 0;
		}
		/* A cdea 04 03 that fails the JOIN matcher is a cold-connect variant
		 * we do not understand — don't guess (never generic UNICAST: that
		 * could falsely close a grant window). The live log dumps the block
		 * so the matcher can be extended from a real capture. */
		return -1;
	}

	if (out->is_broadcast) {
		if (out->kind == REAC_CTRL_FILLER) {
			*ev = REAC_M_RX_BOX_BCAST_FILLER;        /* the presence-flood */
			return 0;
		}
		return -1;   /* another master's probe/announce/… — not a box frame */
	}

	if (!to_us)
		return -1;   /* unicast between other parties */

	/* Unicast-to-us box heartbeat: sel 0x81 keep-alive (the box's ESTABLISHED
	 * "I am locked" signal — symmetric to the heartbeat our slave emits), sel 0x00
	 * disconnect (BYE). */
	if (out->kind == REAC_CTRL_BOX_HB) {
		*ev = (out->sel == 0x00) ? REAC_M_RX_BOX_BYE : REAC_M_RX_BOX_HEARTBEAT;
		return 0;
	}
	/* The box's config-announce (cdea 01 03 0010) is its SETUP DECLARATION — the
	 * frame a mixer enrols the box from. A box that was previously synced does a
	 * WARM RELINK: it skips the flood + cold-connect JOIN and re-appears streaming
	 * unicast, re-declaring itself with this frame (verified live: a real S-0808
	 * to reac-pw-as-master sends config-announce + unicast, never a 04 03 JOIN).
	 * Surface it distinctly so the master FSM can establish on it. */
	if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0010) {
		*ev = REAC_M_RX_BOX_CONFIG;
		return 0;
	}
	/* Any other unicast-to-us box frame — upstream FILLER (628/340 B), unknown
	 * ctrl — proves the box linked to us. */
	*ev = REAC_M_RX_BOX_UNICAST;
	return 0;
}

int reac_box_pin_notice(const char **pin, const char *recognized_token)
{
	if (!pin || !*pin || !recognized_token)
		return 0;
	/* The pin's model token is everything before the optional ":label". */
	size_t toklen = strcspn(*pin, ":");
	int disagrees = strlen(recognized_token) != toklen ||
	                strncmp(*pin, recognized_token, toklen) != 0;
	*pin = NULL;              /* consumed: at most one notice per pin, ever */
	return disagrees;
}

/* ---- The control-frame scaffold + descriptor table ------------------------
 *
 * Every frame reac-pw emits is the same six-step ritual: zero the frame, stamp
 * the ethernet + REAC header, lay the 32-byte control block [18:50], optionally
 * place braided audio at [50:], stamp the checksums, write the C2 EA end marker.
 * What differs between frames is only WHERE the block comes from, whether audio
 * rides along, how wide the frame is and which checksums apply — so those four
 * axes are a TABLE ROW and the ritual is one function. Adding a control frame is
 * a row in CTRL_FRAMES[], not another copied builder.
 *
 * The public builders stay exactly what they were on the wire: this is a pure
 * refactor, byte-for-byte (the goldens are the oracle). */

/* Where a row's 32-byte control block comes from. The BLOCK_* names that select
 * a box-model member are resolved by ctrl_model_block(), so a row names the
 * member and the compiler checks it — no offsets, no casts. */
enum ctrl_block {
	BLOCK_ZERO = 0,   /* left zero — the broadcast presence-flood          */
	BLOCK_DESC,       /* the 00 7a per-slot descriptor — upstream FILLER   */
	BLOCK_TMPL,       /* the row's own literal template                    */
	BLOCK_CONFIG,     /* matrix: config-announce   cdea 01 03 0010         */
	BLOCK_NAME,       /* matrix: ASCII name frame  cdea 04 01 001b         */
	BLOCK_CC0014,     /* matrix: cold-connect      cdea 04 03 0014         */
	BLOCK_CC0013,     /* matrix: cold-connect      cdea 04 03 0013         */
	BLOCK_CC0016,     /* matrix: cold-connect      cdea 04 03 0016         */
	BLOCK_CC001A,     /* matrix: cold-connect      cdea 04 03 001a         */
	BLOCK_EXTRA,      /* matrix: extra frame       cdea 04 02 000d         */
};

/* Frame width. A box->master frame is 50 + 36*width + 2; a master->box frame is
 * the fixed downstream width. */
enum ctrl_len {
	LEN_ARG_WIDTH = 0,   /* the caller's n_ch (validated: even, 2..40)     */
	LEN_MODEL_WIDTH,     /* the matrix row's input width                   */
	LEN_DOWNSTREAM,      /* REAC_FRAME_BYTES (master direction)            */
};

/* Which models emit this frame at all (the 0x84-family name frame, the S-0808
 * 0402000d): a row names the matrix flag, ctrl_gate_ok() reads it. */
enum ctrl_gate {
	GATE_ALWAYS = 0,
	GATE_HAS_NAME,
	GATE_HAS_EXTRA,
};

/* Checksum policy. CKSUM_RECORD means the block carries a Roland DT1 record,
 * whose INNER (sum-to-0x80) checksum lies INSIDE the OUTER (sum-to-0) block
 * checksum — see ctrl_finish() for why the order is the row's whole story. */
enum ctrl_cksum {
	CKSUM_NONE = 0,   /* emitted raw (byte-verified blocks, FILLER)        */
	CKSUM_BLOCK,      /* the OUTER block checksum at [49]                  */
	CKSUM_RECORD,     /* an INNER DT1 record, then the OUTER block         */
};

struct ctrl_frame {
	uint8_t type0, type1;      /* the frame type word at [16:18]            */
	uint8_t block;             /* enum ctrl_block — the block's source      */
	const uint8_t *tmpl;       /* BLOCK_TMPL: the literal block bytes       */
	uint8_t arg_off, arg_len;  /* caller-supplied bytes, block-relative     */
	uint8_t rec_off, rec_len;  /* CKSUM_RECORD: the DT1 record, block-rel.  */
	uint8_t cksum;             /* enum ctrl_cksum                           */
	uint8_t len;               /* enum ctrl_len                             */
	uint8_t gate;              /* enum ctrl_gate                            */
	uint8_t audio;             /* place braided audio at [50:]              */
};

/* The box heartbeat's control block: cdea 01 03 0001, selector 0x81 keep-alive
 * (0x00 is the explicit disconnect). Checksum byte 0x7a on the wire. */
static const uint8_t TMPL_BOX_HB[REAC_CTRL_BLOCK_LEN] = {
	0x01, 0x03, 0x00, 0x01, 0x81,
};

/* The head-amp record container, byte-truthed against a live M-200
 * (m200-headamp-re/ctl2.pcap): cdea 04 03, BE len 0x0013, the f0 41 0a Roland
 * DT1 preamble (block[8] is the preamble length echo, oplen - 5), the 12 12
 * record marker and TAG 01 01 = head-amp. The three zero bytes at block[18:21]
 * are the CH PARAM VALUE window the caller fills; block[21] is the record's
 * INNER checksum, stamped by the scaffold; block[22] is the f7 terminator. */
static const uint8_t TMPL_HEADAMP[REAC_CTRL_BLOCK_LEN] = {
	0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	0x13 - 5, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0xf7,
};
#define HEADAMP_ARG_OFF 18   /* CH PARAM VALUE, block-relative (frame [36:39]) */
#define HEADAMP_ARG_LEN  3
#define HEADAMP_REC_OFF 16   /* TAG..CKSUM,      block-relative (frame [34:40]) */
#define HEADAMP_REC_LEN  6

static const uint8_t *ctrl_model_block(const struct reac_box_model *m,
                                       enum ctrl_block b)
{
	switch (b) {
	case BLOCK_CONFIG: return m->config_block;
	case BLOCK_NAME:   return m->name_block;
	case BLOCK_CC0014: return m->cc0014;
	case BLOCK_CC0013: return m->cc0013;
	case BLOCK_CC0016: return m->cc0016;
	case BLOCK_CC001A: return m->cc001a;
	case BLOCK_EXTRA:  return m->extra_block;
	default:           return NULL;   /* not a matrix block */
	}
}

static int ctrl_gate_ok(const struct reac_box_model *m, enum ctrl_gate g)
{
	switch (g) {
	case GATE_HAS_NAME:  return m->has_name;
	case GATE_HAS_EXTRA: return m->has_extra;
	default:             return 1;
	}
}

/* Lay a row's 32-byte control block [18:50] over `frame`, then overwrite the
 * row's argument window with the caller's bytes. The block is zeroed first, so
 * this is safe over an already-built frame: the audio [50:], the counter and the
 * ethernet header are untouched. */
static void ctrl_lay_block(uint8_t *frame, const struct ctrl_frame *f,
                           const struct reac_box_model *m, const uint8_t *args)
{
	uint8_t *block = frame + REAC_CTRL_BLOCK_OFF;
	const uint8_t *from;

	memset(block, 0, REAC_CTRL_BLOCK_LEN);
	switch ((enum ctrl_block)f->block) {
	case BLOCK_ZERO:
		break;
	case BLOCK_DESC:
		for (int k = 0; k < REAC_CTRL_BLOCK_LEN / 2; k++) {
			block[2 * k]     = DESC_WORD_HI;
			block[2 * k + 1] = DESC_WORD_LO;
		}
		break;
	case BLOCK_TMPL:
		memcpy(block, f->tmpl, REAC_CTRL_BLOCK_LEN);
		break;
	default:
		from = ctrl_model_block(m, (enum ctrl_block)f->block);
		if (from)
			memcpy(block, from, REAC_CTRL_BLOCK_LEN);
		break;
	}
	if (args && f->arg_len)
		memcpy(block + f->arg_off, args, f->arg_len);
}

/* THE CHECKSUM ORDER, MADE STRUCTURAL.
 *
 * An op-0403 record carries a Roland DT1 checksum (INNER, sum-to-0x80) INSIDE
 * the REAC control block's own checksum (OUTER, sum-to-0). The inner byte is one
 * of the bytes the outer sum covers, so it MUST be stamped first — a record
 * finished with only the block helper, or with the block helper first, looks
 * perfect on the wire and the box rejects it.
 *
 * This function is the only place in reac_ctrl that stamps either checksum, and
 * CKSUM_RECORD falls through into CKSUM_BLOCK. A table row therefore cannot ask
 * for the outer checksum alone on a frame that carries a record, and cannot ask
 * for them in the wrong order: the order is not a convention a builder has to
 * remember, it is the only path through the switch. */
static void ctrl_finish(uint8_t *frame, const struct ctrl_frame *f)
{
	switch ((enum ctrl_cksum)f->cksum) {
	case CKSUM_RECORD:
		reac_ctrl_record_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF + f->rec_off,
		                             f->rec_len);
		/* fall through - the outer checksum covers the byte just stamped */
	case CKSUM_BLOCK:
		reac_ctrl_block_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF);
		break;
	case CKSUM_NONE:
		break;
	}
}

/* The six-step ritual, once. Returns the frame length, or 0 when the row is not
 * emitted for this model / the width is not a real box width. */
static size_t ctrl_emit(uint8_t *out, const struct ctrl_frame *f,
                        const uint8_t dst[6], const uint8_t src[6],
                        uint16_t counter, int n_ch, const uint8_t *args,
                        float *const *planar, int ns)
{
	/* The braid packs channel PAIRS: box widths are even, 2..40 (628 B at 16,
	 * 340 B at 8). Rows sized from the matrix carry a verified width already. */
	if (f->len == LEN_ARG_WIDTH &&
	    (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1)))
		return 0;

	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	if (!ctrl_gate_ok(m, (enum ctrl_gate)f->gate))
		return 0;

	size_t len = (f->len == LEN_DOWNSTREAM)
	           ? (size_t)REAC_FRAME_BYTES
	           : reac_ctrl_box_frame_len(f->len == LEN_MODEL_WIDTH ? m->in_ch : n_ch);

	memset(out, 0, len);
	put_hdr(out, dst, src, counter, f->type0, f->type1);
	ctrl_lay_block(out, f, m, args);
	if (f->audio)
		reac_braid_encode(out + AUDIO_OFF, n_ch, planar, n_ch, ns);
	ctrl_finish(out, f);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	return len;
}

/* Lay a row over an ALREADY-BUILT frame: the type word [16:18] and the control
 * block [18:50] change, the audio [50:], the counter and the C2 EA tail the
 * frame already carries are preserved. Shares ctrl_lay_block + ctrl_finish with
 * ctrl_emit, so an overlaid record gets its two checksums in the same order a
 * freshly built one does — the stamp path cannot drift from the build path. */
static void ctrl_stamp(uint8_t *frame, const struct ctrl_frame *f, const uint8_t *args)
{
	frame[TYPE_OFF] = f->type0;
	frame[TYPE_OFF + 1] = f->type1;
	ctrl_lay_block(frame, f, NULL, args);
	ctrl_finish(frame, f);
}

/* The table. One row per control frame; the evidence for each block lives with
 * the bytes (TMPL_* here, or the box-model matrix above). */
enum ctrl_frame_id {
	CTRL_BOX_HB = 0,
	CTRL_UPSTREAM_FILLER,
	CTRL_FLOOD_FILLER,
	CTRL_CONFIG_ANNOUNCE,
	CTRL_NAME_FRAME,
	CTRL_COLDCONNECT,
	CTRL_COLDCONNECT_0013,
	CTRL_COLDCONNECT_0016,
	CTRL_COLDCONNECT_001A,
	CTRL_EXTRA_FRAME,
	CTRL_HEADAMP,
	CTRL_FRAME_COUNT,
};

/* Rows CTRL_CONFIG_ANNOUNCE..CTRL_EXTRA_FRAME are the RECONSTRUCTED JOIN frames
 * (experimental, not byte-verified as a SEQUENCE): each block is byte-matched to
 * a real capture, but the order and timing a box emits them in is reconstructed
 * from REAC-CONNECTION-FSM.md, not observed end to end. */
static const struct ctrl_frame CTRL_FRAMES[CTRL_FRAME_COUNT] = {
	/* The box keep-alive, in a box-width audio slot. Checksum byte 0x7a on the
	 * wire (reac-captures/wired-reac-a-bothdirs) — the test cross-checks it. */
	[CTRL_BOX_HB] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_TMPL, .tmpl = TMPL_BOX_HB,
		.cksum = CKSUM_BLOCK, .len = LEN_ARG_WIDTH },
	/* The established unicast upstream: the 32-byte descriptor [18:50] = 00 7a per
	 * slot (16 slots), as the real box, over audio in the BRAIDED layout (resolved
	 * 2026-07-10, task #108). FILLER (type 00 00) is checksum-exempt. */
	[CTRL_UPSTREAM_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_DESC,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The presence-flood FILLER (broadcast, unlinked): counter + type 00 00 + a
	 * ZERO control block [18:50] (no 0x7a per-slot descriptor) + LIVE audio
	 * [50:626] + end marker. Verified on the wire
	 * (m200-s1608-realbox-establish-2026-07-11.pcap): a real S-1608's cold-boot
	 * flood carries a zero control block but a LIVE audio region (it varies every
	 * frame) — it is NOT an all-zero payload. The 0x7a descriptor is what
	 * distinguishes the ESTABLISHED unicast upstream from this broadcast announce;
	 * the audio itself is present in both. */
	[CTRL_FLOOD_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_ZERO,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The config-announce (cdea 01 03 0010) — the SETUP DECLARATION the master
	 * enrols the box from; the selector byte sets the displayed model family. The
	 * verified blocks already sum to 0, so the outer stamp is a no-op that keeps
	 * the invariant rather than a correction. */
	[CTRL_CONFIG_ANNOUNCE] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CONFIG,
		.cksum = CKSUM_BLOCK, .len = LEN_MODEL_WIDTH },
	/* The ASCII model-name frame (cdea 04 01 001b) — required for the 0x84 family
	 * so the desk shows the exact model ("S-0808") instead of the generic family
	 * name. The 0x82 / S-1608 family is named by its selector alone and emits
	 * nothing here. */
	[CTRL_NAME_FRAME] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_NAME,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_NAME },
	/* The cold-connect escalation a real S-1608 sends: 0014 -> 0013 -> 0016 ->
	 * 001a, each the model's 32-byte control block over LIVE audio. The block's
	 * [38:66] region is frame[52:80] and is AUDIO, not device inventory — on a
	 * real box it varies every frame (verified 2026-07-11,
	 * m200-s1608-realbox-establish). The master needs no inventory tail: it learns
	 * the box from the L2 source and echoes THIS block back verbatim as its grant,
	 * so a cold-connect is the control block over live audio, exactly like the
	 * unicast upstream but with cdea 04 03 replacing the 0x7a descriptor.
	 *
	 * 0014: the 0x41 at block[10] is descriptor DATA, not a MAC tail — the block
	 * is MAC-independent. Sum(block) mod 256 == 0 holds as captured, so the outer
	 * stamp is a no-op that keeps the invariant.
	 * 0013: the variant a real box INTERLEAVES with the 0014 (S-1608 cold boot,
	 * m200-s1608-BIDIR-reboot-2026-07-11); block[31]=0x02 trailer, and the
	 * captured block sums to 0xfe mod 256 — NOT sum-to-0, which is the evidence
	 * that the cold-connect is not checksum-validated the way 0014 happens to be.
	 * It is therefore emitted RAW, as are 0016 and 001a.
	 * 0016: a MODEL-specific inventory block the mixer uses to identify the box.
	 * 001a: the fullest MODEL-specific box inventory.
	 * All four byte-matched per model (matrix-m200-s1608 / -s0808, 2026-07-11;
	 * S-4000S from s4000s-coldboot-m5000-2026-07-12). */
	[CTRL_COLDCONNECT] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0014,
		.cksum = CKSUM_BLOCK, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_0013] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0013,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_0016] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC0016,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	[CTRL_COLDCONNECT_001A] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CC001A,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The cdea 04 02 000d frame some models (S-0808) send during cold-connect —
	 * part of the inventory the mixer reads to name the exact model. Emitted raw
	 * (byte-verified, matrix-m200-s0808); models without it emit nothing. */
	[CTRL_EXTRA_FRAME] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_EXTRA,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_EXTRA },
	/* The console-side preamp command, master->box at the downstream width. The
	 * ONLY row that carries a DT1 record — and naming CKSUM_RECORD is all it has
	 * to do: ctrl_finish() stamps the inner checksum and then the outer one, in
	 * that order, because that is the only path through its switch. */
	[CTRL_HEADAMP] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_TMPL, .tmpl = TMPL_HEADAMP,
		.arg_off = HEADAMP_ARG_OFF, .arg_len = HEADAMP_ARG_LEN,
		.rec_off = HEADAMP_REC_OFF, .rec_len = HEADAMP_REC_LEN,
		.cksum = CKSUM_RECORD, .len = LEN_DOWNSTREAM },
};

size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter, int n_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_BOX_HB], master, src, counter,
	                 n_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_UPSTREAM_FILLER], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_flood_filler(uint8_t *out, const uint8_t bcast[6],
                                    const uint8_t src[6], uint16_t counter,
                                    int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_FLOOD_FILLER], bcast, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_CONFIG_ANNOUNCE], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_name_frame(uint8_t *out, const uint8_t master[6],
                                  const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_NAME_FRAME], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_0013(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_0013], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_0016(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_0016], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_coldconnect_001a(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_COLDCONNECT_001A], master, src,
	                 counter, n_ch, NULL, planar, ns);
}

size_t reac_ctrl_build_extra_frame(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter, int in_ch)
{
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_EXTRA_FRAME], master, src,
	                 counter, in_ch, NULL, NULL, 0);
}

/* ---- Head-amp source control (op 04 03, record TAG 01 01) ---- */

/* param/value validity for a head-amp record (phantom/pad are boolean, SENS is
 * 0x00..0x37). Shared by the fresh-frame builder and the in-place stamp. */
static int headamp_args_ok(uint8_t param, uint8_t value)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		return value <= 0x01;
	case REAC_HEADAMP_SENS:
		return value <= REAC_HEADAMP_SENS_MAX;
	default:
		return 0;
	}
}

size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value)
{
	/* A real console BROADCASTS these interleaved in its stream, so the caller
	 * passes the broadcast MAC like every builder's first MAC arg. */
	const uint8_t args[HEADAMP_ARG_LEN] = { ch, param, value };

	if (!headamp_args_ok(param, value))
		return 0;
	return ctrl_emit(out, &CTRL_FRAMES[CTRL_HEADAMP], master, src, counter,
	                 0, args, NULL, 0);
}

int reac_ctrl_stamp_headamp(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value)
{
	/* Overlay a head-amp record onto an already-built downstream frame (the
	 * MASTER-role emit path stamps it over a FILLER slot — see reac_headamp_tx).
	 * Only the type [16:18] + control block [18:50] change; the audio, counter and
	 * C2/EA tail the frame already carries are preserved. Returns -1 on a bad
	 * param/value, leaving the frame untouched. */
	const uint8_t args[HEADAMP_ARG_LEN] = { ch, param, value };

	if (!headamp_args_ok(param, value))
		return -1;
	ctrl_stamp(frame, &CTRL_FRAMES[CTRL_HEADAMP], args);
	return 0;
}

const char *reac_headamp_param_name(uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM: return "phantom";
	case REAC_HEADAMP_PAD:     return "pad";
	case REAC_HEADAMP_SENS:    return "SENS";
	default:                   return "?";
	}
}

int reac_ctrl_headamp_record_verify(const uint8_t *frame)
{
	/* The inner record is TAG(2) CH PARAM VALUE CKSUM at frame[34..39]; the
	 * console builds CKSUM so the six bytes sum to 0x80 mod 256 (byte-verified
	 * on the M-200, m200-headamp-re/DECODE.md). A frame that fails this carries a
	 * corrupted preamp record and its CH/PARAM/VALUE must not be trusted. */
	return reac_ctrl_record_cksum_verify(frame + 34, 6);
}

/* SENS dB <-> VALUE (pad-relative, 1 dB/step): dB = -10 - value + (pad ? 20 : 0).
 * Ground-truthed on the M-200 SENS display: pad off 0x00 = -10 dBu .. 0x37 =
 * -65 dBu; pad on 0x00 = +10 .. 0x37 = -45. */

