// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_ctrl.h"
#include <reac/reac.h>
#include <reac/reac_braid.h>   /* reac_braid_pos — the layout oracle */
#include <reac/reac_sample.h>  /* reac_f32_to_s24le */
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

void reac_ctrl_block_cksum_stamp(uint8_t block[REAC_CTRL_BLOCK_LEN])
{
	unsigned s = 0;
	for (int i = 0; i < REAC_CTRL_BLOCK_LEN - 1; i++)
		s += block[i];
	block[REAC_CTRL_BLOCK_LEN - 1] = (uint8_t)((256 - (s & 0xff)) & 0xff);
}

void reac_ctrl_record_cksum_stamp(uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i + 1 < n; i++)
		s += rec[i];
	rec[n - 1] = (uint8_t)((0x80 - s) & 0xff);
}

int reac_ctrl_record_cksum_verify(const uint8_t *rec, size_t n)
{
	unsigned s = 0;
	for (size_t i = 0; i < n; i++)
		s += rec[i];
	return ((s & 0xff) == 0x80) ? 0 : -1;
}

void reac_ctrl_checksum_apply(uint8_t *frame)
{
	reac_ctrl_block_cksum_stamp(frame + REAC_CTRL_BLOCK_OFF);
}

int reac_ctrl_checksum_verify(const uint8_t *frame)
{
	unsigned s = 0;
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_BLOCK_END; i++)
		s += frame[i];
	return (s & 0xff) == 0 ? 0 : -1;
}

enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out)
{
	memset(out, 0, sizeof *out);
	if (len < AUDIO_OFF || frame[12] != 0x88 || frame[13] != 0x19) {
		out->kind = REAC_CTRL_NONE;
		return out->kind;
	}
	memcpy(out->dst, frame, 6);
	memcpy(out->src, frame + 6, 6);
	out->is_broadcast = (memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6) == 0);
	out->counter = (uint16_t)(frame[CNT_OFF] | (frame[CNT_OFF + 1] << 8));
	out->op0 = frame[18]; out->op1 = frame[19];
	out->op_len = (uint16_t)((frame[20] << 8) | frame[21]);
	out->sel = frame[22];
	out->sel2 = frame[23];

	const uint8_t t0 = frame[TYPE_OFF], t1 = frame[TYPE_OFF + 1];
	if (t0 == 0x00 && t1 == 0x00) {
		out->kind = REAC_CTRL_FILLER;
	} else if (t0 == 0xcf && t1 == 0xea) {
		out->kind = REAC_CTRL_MASTER_ANNOUNCE;
	} else if (t0 == 0xcd && t1 == 0xea) {
		if (out->op0 == 0x04 && out->op1 == 0x03) {
			/* op 04 03 is a RECORD CONTAINER, not one opcode: after the
			 * 12 12 marker at [32] comes a 2-byte TAG. TAG 01 00 = the
			 * connect-grant; TAG 01 01 = a HEAD-AMP record (CH PARAM
			 * VALUE) — a live M-200 emits ~628 head-amp records per 14
			 * grants, so a joining slave must NOT read a preamp
			 * knob-turn as its grant. Every other tag (03 02, 05 00,
			 * 00 00 — the cold-connect inventory variants) stays GRANT
			 * as before (ground truth: m200-headamp-re/DECODE.md). */
			if (frame[32] == 0x12 && frame[33] == 0x12 &&
			    frame[34] == 0x01 && frame[35] == 0x01) {
				out->kind = REAC_CTRL_HEADAMP;
				out->ch    = frame[36];
				out->param = frame[37];
				out->value = frame[38];
			} else {
				out->kind = REAC_CTRL_GRANT;
			}
		} else if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0019)
			out->kind = REAC_CTRL_MASTER_HB;       /* master established heartbeat */
		else if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0001)
			out->kind = REAC_CTRL_BOX_HB;          /* a box keep-alive */
		else if (out->op0 == 0x01)
			out->kind = REAC_CTRL_PROBE;           /* master hunting (sub-states) */
		else
			out->kind = REAC_CTRL_UNKNOWN_CTRL;
	} else {
		out->kind = REAC_CTRL_UNKNOWN_CTRL;
	}
	return out->kind;
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

/* box-width frame length for n_ch inputs */
static size_t box_frame_len(int n_ch)
{
	return (size_t)AUDIO_OFF + (size_t)n_ch * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION + 2;
}

/* Place n_ch planar float channels (ns samples each) into the box's braided audio
 * region at `audio` (frame[50:..]), the exact layout reac_upstream_decode() inverts
 * (task #108, the ex-"FPGA scramble" of task #61). Byte positions come from
 * libreac's reac_braid_pos() — the single layout oracle (<reac/reac_braid.h> has
 * the byte map + evidence). Slot placement is plain ascending. A real M-5000
 * expects exactly this from a box's return. Shared by EVERY box->master frame
 * that carries audio — the upstream FILLER, the broadcast presence-flood, AND
 * the cold-connect — because on a real box the audio region varies every frame
 * (it is live input, NOT static inventory). */
static void place_braided_audio(uint8_t *audio, int n_ch, float *const *planar, int ns)
{
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++)
		for (int ch = 0; ch < n_ch; ch++) {
			float v = planar && planar[ch] ? planar[ch][s] : 0.0f;
			uint8_t s24[3];
			size_t pos[3];
			reac_f32_to_s24le(v, s24);
			reac_braid_pos(s, ch, n_ch, pos);
			audio[pos[0]] = s24[0]; audio[pos[1]] = s24[1]; audio[pos[2]] = s24[2];
		}
}

/* ---- FIXED box-model matrix (byte-verified real announce blocks) ----
 * Role decides authority (docs/REAC-BOX-STATE-DIAGRAM.md): as a SLAVE (we ARE a
 * stagebox) this matrix is LAW — we pick a row and emit its announce verbatim. As
 * a MASTER (we ARE a mixer) the box's announce on the wire is the truth and this
 * matrix is only a default. Each row is a real box's captured config-announce
 * (selector byte = displayed model family; sum mod 256 == 0 with its trailing
 * check byte), plus, for the 0x84 family, the ASCII name frame that names the
 * exact model. All blocks byte-matched to matrix-m200/m5000-s1608 / -s0808. */
static const struct reac_box_model BOX_MODELS[] = {
	{ .token = "s1608", .display = "S-1608 (16 in / 8 out)", .in_ch = 16, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x82, 0x00, 0x00, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_name = 0,     /* 0x82 family: named by selector, no ASCII frame */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x03, 0x00, 0x02, 0x6e, 0xf7, 0x00, 0xf4 },
	  .has_extra = 0 },  /* S-1608 sends no 0402000d */
	{ .token = "s0808", .display = "S-0808 (8 in / 8 out)", .in_ch = 8, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x01, 0x01, 0x03, 0x03, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a },
	  .has_name = 1,     /* 0x84 family: ASCII name frame gives the exact model */
	  .name_block = {
		0x04, 0x01, 0x00, 0x1b, 0x00, 0x02, 0x00, 0xfe,
		0x16, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x10, 0x00, 0x01, 0x53, 0x2d, 0x30,   /* "S-0" */
		0x38, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x05 },  /* "808" */
	  .cc0014 = {         /* 0014/0013 match the S-1608's (model-generic so far) */
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {         /* S-0808's inventory differs from S-1608's */
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03,
		0x77, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x74, 0xf7, 0x00, 0xf4 },
	  .has_extra = 1,     /* S-0808 also sends cdea 04 02 000d */
	  .extra_block = {
		0x04, 0x02, 0x00, 0x0d, 0x00, 0x02, 0x00, 0xfe,
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a,
		0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd4 } },
	/* S-4000S — also 0x84 family but sends NO name frame (0x84's DEFAULT desk
	 * label IS "S-4000S") and NO 0402000d. Its config descriptor + 0016/001a
	 * inventory are distinct. Byte-verified from a real S-4000S cold boot on an
	 * M-5000 (s4000s-coldboot-m5000-2026-07-12, box c4:06:80). NOTE: captured on
	 * OHRCA (frames +2 CRC trailer); the control blocks below are generation-
	 * independent, but emulating on an OHRCA desk needs the upstream +2 (W4). */
	{ .token = "s4000s", .display = "S-4000S (32 in / 8 out)", .in_ch = 32, .out_ch = 8,
	  .config_block = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x01, 0x01, 0x03, 0x03, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c },
	  .has_name = 0,     /* 0x84 DEFAULT name is "S-4000S" — no ASCII frame */
	  .cc0014 = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	  .cc0013 = {
		0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
		0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x03, 0x02, 0x00, 0x01, 0x00, 0x7a, 0xf7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
	  .cc0016 = {
		0x04, 0x03, 0x00, 0x16, 0x00, 0x02, 0x00, 0xfe,
		0x11, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x00, 0x00, 0x02, 0x05, 0x00, 0x00,
		0x74, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc },
	  .cc001a = {
		0x04, 0x03, 0x00, 0x1a, 0x00, 0x02, 0x00, 0xfe,
		0x15, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x01, 0x00, 0x02, 0x70, 0xf7, 0x00, 0xf4 },
	  .has_extra = 0 },  /* S-4000S sends no 0402000d */
};

const struct reac_box_model *reac_box_model_table(size_t *count)
{
	if (count) *count = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	return BOX_MODELS;
}

const struct reac_box_model *reac_box_model_by_token(const char *token)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (token && strcmp(BOX_MODELS[i].token, token) == 0)
			return &BOX_MODELS[i];
	return NULL;
}

/* Map an input width to its matrix row (each verified width is one model). Falls
 * back to S-1608 for widths not in the matrix so the pure builders never fault. */
const struct reac_box_model *reac_box_model_by_channels(int in_ch)
{
	size_t n = sizeof(BOX_MODELS) / sizeof(BOX_MODELS[0]);
	for (size_t i = 0; i < n; i++)
		if (BOX_MODELS[i].in_ch == in_ch)
			return &BOX_MODELS[i];
	return &BOX_MODELS[0];   /* default: S-1608 */
}

const struct reac_box_model *reac_ctrl_identify_box(const uint8_t *frame, size_t len)
{
	/* Recognize the connected box's MODEL from its config-announce
	 * (cdea 01 03 0010) by matching the 32-byte descriptor block against the
	 * fixed matrix. Each row's config_block is unique (selector + descriptor:
	 * S-1608 0x82; S-0808 / S-4000S both 0x84 but distinct descriptors), so an
	 * exact block match uniquely names the model. NULL = not a config-announce,
	 * or no known model -> caller falls back to the frame's own descriptor/width. */
	if (len < REAC_CTRL_BLOCK_OFF + 32)               return NULL;
	if (frame[12] != 0x88 || frame[13] != 0x19)       return NULL;   /* 0x8819    */
	if (frame[16] != 0xcd || frame[17] != 0xea)       return NULL;   /* cdea      */
	if (frame[18] != 0x01 || frame[19] != 0x03 ||
	    frame[20] != 0x00 || frame[21] != 0x10)       return NULL;   /* 01 03 0010 */
	size_t n; const struct reac_box_model *t = reac_box_model_table(&n);
	for (size_t i = 0; i < n; i++)
		if (memcmp(frame + REAC_CTRL_BLOCK_OFF, t[i].config_block, 32) == 0)
			return &t[i];
	return NULL;
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
	           : box_frame_len(f->len == LEN_MODEL_WIDTH ? m->in_ch : n_ch);

	memset(out, 0, len);
	put_hdr(out, dst, src, counter, f->type0, f->type1);
	ctrl_lay_block(out, f, m, args);
	if (f->audio)
		place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	ctrl_finish(out, f);
	out[len - 2] = REAC_END_MARKER_0;
	out[len - 1] = REAC_END_MARKER_1;
	return len;
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
	CTRL_FRAME_COUNT,
};

static const struct ctrl_frame CTRL_FRAMES[CTRL_FRAME_COUNT] = {
	/* The box keep-alive, in a box-width audio slot. */
	[CTRL_BOX_HB] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_TMPL, .tmpl = TMPL_BOX_HB,
		.cksum = CKSUM_BLOCK, .len = LEN_ARG_WIDTH },
	/* The established unicast upstream: the 00 7a per-slot descriptor over live
	 * braided audio. FILLER (type 00 00) is checksum-exempt. */
	[CTRL_UPSTREAM_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_DESC,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The cold-boot presence-flood: broadcast, ZERO control block (no 0x7a
	 * descriptor) but LIVE audio — verified on the wire
	 * (m200-s1608-realbox-establish-2026-07-11), it is NOT an all-zero payload.
	 * The descriptor is what distinguishes the established upstream from this. */
	[CTRL_FLOOD_FILLER] = {
		.type0 = 0x00, .type1 = 0x00, .block = BLOCK_ZERO,
		.cksum = CKSUM_NONE, .len = LEN_ARG_WIDTH, .audio = 1 },
	/* The SETUP DECLARATION the master enrols the box from. The verified blocks
	 * already sum to 0, so the outer stamp is a no-op that keeps the invariant. */
	[CTRL_CONFIG_ANNOUNCE] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_CONFIG,
		.cksum = CKSUM_BLOCK, .len = LEN_MODEL_WIDTH },
	/* The ASCII model name — 0x84 family only (0x82 is named by its selector). */
	[CTRL_NAME_FRAME] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_NAME,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_NAME },
	/* The cold-connect escalation a real S-1608 sends: 0014 -> 0013 -> 0016 ->
	 * 001a, each the 32-byte control block over LIVE audio (the [38:66] region is
	 * per-frame audio, NOT device inventory — verified 2026-07-11). The master
	 * learns the box from the L2 source and echoes the block back as its grant.
	 * Only the 0014 block is sum-to-0; the rest are emitted raw as captured
	 * (0013 sums to 0xfe), which is itself the evidence that the cold-connect is
	 * not checksum-validated the way 0014 happens to be. */
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
	/* The extra inventory frame some models send (S-0808). */
	[CTRL_EXTRA_FRAME] = {
		.type0 = 0xcd, .type1 = 0xea, .block = BLOCK_EXTRA,
		.cksum = CKSUM_NONE, .len = LEN_MODEL_WIDTH, .gate = GATE_HAS_EXTRA },
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

/* Write the head-amp type [16:18] + control block [18:50] into `frame` (a 0013
 * record container: TAG 01 01 + CH PARAM VALUE + the INNER record checksum),
 * then apply the OUTER block checksum at [49]. The control block is zeroed first,
 * so this is safe to STAMP over an existing FILLER frame — the audio region
 * [50:], the counter [14:16] and the ethernet header are untouched. Assumes the
 * args have already passed headamp_args_ok. */
static void put_headamp_block(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value)
{
	frame[16] = 0xcd; frame[17] = 0xea;       /* control-frame type */
	memset(frame + REAC_CTRL_BLOCK_OFF, 0, 32);/* clear the 32-byte block [18:50] */
	frame[18] = 0x04; frame[19] = 0x03;       /* the record container */
	frame[20] = 0x00; frame[21] = 0x13;       /* BE len 0x0013 */
	frame[22] = 0x00; frame[23] = 0x02;
	frame[24] = 0x00; frame[25] = 0xfe;
	frame[26] = 0x13 - 5;                     /* preamble length echo: oplen - 5 */
	frame[27] = 0xf0; frame[28] = 0x41; frame[29] = 0x0a;   /* f0 41 0a 00 00 */
	frame[32] = 0x12; frame[33] = 0x12;       /* record marker */
	frame[34] = 0x01; frame[35] = 0x01;       /* TAG 01 01 = head-amp */
	frame[36] = ch; frame[37] = param; frame[38] = value;
	/* INNER record checksum: TAG..CKSUM sums to 0x80 mod 256 (the general
	 * record rule — see reac_ctrl_record_cksum_stamp). */
	reac_ctrl_record_cksum_stamp(frame + 34, 6);
	frame[40] = 0xf7;                         /* record terminator */
	reac_ctrl_checksum_apply(frame);          /* OUTER block checksum at [49] */
}

size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value)
{
	/* The console-side preamp command, byte-truthed against a live M-200
	 * (m200-headamp-re/ctl2.pcap): a 0013 record container whose record is
	 * TAG 01 01 + CH PARAM VALUE + the INNER record checksum, over a zero
	 * audio region at the downstream (master) width. */
	if (!headamp_args_ok(param, value))
		return 0;
	size_t len = REAC_FRAME_BYTES;        /* master frames are downstream width */
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	put_headamp_block(out, ch, param, value);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

int reac_ctrl_stamp_headamp(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value)
{
	/* Overlay a head-amp record onto an already-built downstream frame (the
	 * MASTER-role emit path stamps it over a FILLER slot — see reac_headamp_tx).
	 * Only the type [16:18] + control block [18:50] change; the audio, counter and
	 * C2/EA tail the frame already carries are preserved. */
	if (!headamp_args_ok(param, value))
		return -1;
	put_headamp_block(frame, ch, param, value);
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
int reac_headamp_sens_db(uint8_t value, int pad_on)
{
	return -10 - (int)value + (pad_on ? 20 : 0);
}

uint8_t reac_headamp_sens_value(int db, int pad_on)
{
	int v = -10 - db + (pad_on ? 20 : 0);
	if (v < 0)
		v = 0;
	if (v > REAC_HEADAMP_SENS_MAX)
		v = REAC_HEADAMP_SENS_MAX;
	return (uint8_t)v;
}
