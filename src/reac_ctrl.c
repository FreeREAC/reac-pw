// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_ctrl.h"
#include <reac/reac.h>
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

static inline void f32_to_s24le(float v, uint8_t *p)
{
	float x = v * 8388608.0f;
	if (x > 8388607.0f) x = 8388607.0f;
	if (x < -8388608.0f) x = -8388608.0f;
	int32_t s = (int32_t)lrintf(x);
	p[0] = (uint8_t)(s & 0xFF); p[1] = (uint8_t)((s >> 8) & 0xFF); p[2] = (uint8_t)((s >> 16) & 0xFF);
}

void reac_ctrl_checksum_apply(uint8_t *frame)
{
	unsigned s = 0;
	for (int i = REAC_CTRL_BLOCK_OFF; i < REAC_CTRL_CKSUM_OFF; i++)
		s += frame[i];
	frame[REAC_CTRL_CKSUM_OFF] = (uint8_t)((256 - (s & 0xff)) & 0xff);
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
 * (task #108, the ex-"FPGA scramble" of task #61): per time sample each channel PAIR
 * shares a 6-byte group; the even channel's s24 LE (lo,mid,hi) bytes sit at
 * group[3],group[0],group[1] and the odd channel's at group[4],group[5],group[2].
 * Slot placement is plain ascending. A real M-5000 expects exactly this from a box's
 * return. Shared by EVERY box->master frame that carries audio — the upstream FILLER,
 * the broadcast presence-flood, AND the cold-connect — because on a real box the audio
 * region varies every frame (it is live input, NOT static inventory). */
static void place_braided_audio(uint8_t *audio, int n_ch, float *const *planar, int ns)
{
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++)
		for (int ch = 0; ch < n_ch; ch++) {
			float v = planar && planar[ch] ? planar[ch][s] : 0.0f;
			uint8_t s24[3];
			f32_to_s24le(v, s24);
			uint8_t *g = audio + (size_t)s * n_ch * REAC_RESOLUTION
			                   + (size_t)(ch & ~1) * REAC_RESOLUTION;
			if ((ch & 1) == 0) {
				g[3] = s24[0]; g[0] = s24[1]; g[1] = s24[2];
			} else {
				g[4] = s24[0]; g[5] = s24[1]; g[2] = s24[2];
			}
		}
}

size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter, int n_ch)
{
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;                        /* box widths are even 2..40 (628B@16, 340B@8) */
	size_t len = box_frame_len(n_ch);    /* the heartbeat occupies a box-width audio slot */
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);   /* unicast cdea */
	out[18] = 0x01; out[19] = 0x03;       /* cdea 01 03 */
	out[20] = 0x00; out[21] = 0x01;       /* BE len 0x0001 */
	out[22] = 0x81;                       /* keep-alive selector (0x00 = disconnect) */
	reac_ctrl_checksum_apply(out);        /* sets out[49] */
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns)
{
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;  /* the braid packs channel PAIRS; odd widths don't exist on-wire */
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0x00, 0x00);   /* unicast FILLER */
	/* 32-byte descriptor [18:50] = 00 7a per slot (16 slots), as the real box. */
	for (int k = 0; k < 16; k++) {
		out[18 + 2 * k] = DESC_WORD_HI;
		out[18 + 2 * k + 1] = DESC_WORD_LO;
	}
	/* audio [50:..] in the box's BRAIDED layout (resolved 2026-07-10, task #108). */
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;                            /* FILLER: no checksum (exempt) */
}

/* The presence-flood FILLER (broadcast, unlinked): counter + type 00 00 + a ZERO
 * control block [18:50] (no 0x7a per-slot descriptor) + LIVE audio [50:626] + end
 * marker. Verified on the wire (m200-s1608-realbox-establish-2026-07-11.pcap): a
 * real S-1608's cold-boot flood carries a zero control block but a LIVE audio
 * region (it varies every frame) — it is NOT an all-zero payload. The 0x7a
 * descriptor is what distinguishes the ESTABLISHED unicast upstream from this
 * broadcast announce; the audio itself is present in both. */
size_t reac_ctrl_build_flood_filler(uint8_t *out, const uint8_t bcast[6],
                                    const uint8_t src[6], uint16_t counter,
                                    int n_ch, float *const *planar, int ns)
{
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, bcast, src, counter, 0x00, 0x00);   /* broadcast FILLER, zero block */
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
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

/* ---- RECONSTRUCTED JOIN builders (experimental, not byte-verified) ---- */

size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch)
{
	const struct reac_box_model *m = reac_box_model_by_channels(in_ch);
	size_t len = box_frame_len(m->in_ch);       /* width-matched: S-1608 628 B / S-0808 340 B */
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->config_block, 32);
	reac_ctrl_checksum_apply(out);              /* no-op: verified blocks already sum to 0 */
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_name_frame(uint8_t *out, const uint8_t master[6],
                                  const uint8_t src[6], uint16_t counter, int in_ch)
{
	const struct reac_box_model *m = reac_box_model_by_channels(in_ch);
	if (!m->has_name)
		return 0;                               /* 0x82 family: named by selector, no frame */
	size_t len = box_frame_len(m->in_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->name_block, 32);  /* emitted raw (byte-verified) */
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns)
{
	/* The cold-connect cdea 04 03 0014 block — from the matrix per model. The 0x41
	 * at block[10] is descriptor DATA, not a MAC tail (the block is MAC-independent;
	 * the master learns the box from the L2 source and echoes the block as its
	 * grant). Sum(block) mod 256 == 0 holds as captured. */
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->cc0014, 32);
	/* payload[38:66] = frame[52:80] is AUDIO, not device inventory: on a real box
	 * that region varies every frame (verified 2026-07-11,
	 * m200-s1608-realbox-establish). The master needs NO inventory tail — it learns
	 * the box from the L2 source and echoes THIS 32-byte control block back verbatim
	 * as the grant. So the cold-connect is the control block over LIVE audio, exactly
	 * like the unicast upstream but with cdea 04 03 replacing the 0x7a descriptor. */
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	reac_ctrl_checksum_apply(out);        /* no-op by construction (block sums 0) */
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

/* The cdea 04 03 0013 cold-connect variant a real box INTERLEAVES with the 0014
 * (S-1608 cold boot, m200-s1608-BIDIR-reboot-2026-07-11): same framing, a distinct
 * 32-byte control block with BE len 0x0013 and its own descriptor. It is NOT
 * sum-to-0 (the captured block sums to 0xfe mod 256), which proves the cold-connect
 * is not checksum-validated the way the 0014 block happens to be — so it is emitted
 * RAW (no checksum_apply). Over live braided audio like every box->master frame. */
size_t reac_ctrl_build_coldconnect_0013(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	/* cdea 04 03 0013 — from the matrix per model (block[31]=0x02 trailer; not
	 * sum-to-0, so emitted RAW). Byte-matched per model. */
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->cc0013, 32);
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_coldconnect_0016(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	/* The third cold-connect variant (cdea 04 03, BE len 0x0016): a MODEL-specific
	 * inventory block the mixer uses to identify the box. Byte-matched per model
	 * (matrix-m200-s1608 / -s0808, 2026-07-11). */
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->cc0016, 32);
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_coldconnect_001a(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns)
{
	/* The fourth/final cold-connect variant (cdea 04 03, BE len 0x001a) — the fullest
	 * MODEL-specific box inventory. Byte-matched per model (matrix-m200 captures). */
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	const struct reac_box_model *m = reac_box_model_by_channels(n_ch);
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->cc001a, 32);
	place_braided_audio(out + AUDIO_OFF, n_ch, planar, ns);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_extra_frame(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter, int in_ch)
{
	/* The cdea 04 02 000d frame some models (S-0808) send during cold-connect —
	 * part of the inventory the mixer reads to name the exact model. Emitted raw
	 * (byte-verified, matrix-m200-s0808). Returns 0 for models without it. */
	const struct reac_box_model *m = reac_box_model_by_channels(in_ch);
	if (!m->has_extra)
		return 0;
	size_t len = box_frame_len(m->in_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, m->extra_block, 32);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

/* ---- Head-amp source control (op 04 03, record TAG 01 01) ---- */

size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value)
{
	/* The console-side preamp command, byte-truthed against a live M-200
	 * (m200-headamp-re/ctl2.pcap): a 0013 record container whose record is
	 * TAG 01 01 + CH PARAM VALUE + the INNER record checksum, over a zero
	 * audio region at the downstream (master) width. */
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		if (value > 0x01)
			return 0;
		break;
	case REAC_HEADAMP_SENS:
		if (value > REAC_HEADAMP_SENS_MAX)
			return 0;
		break;
	default:
		return 0;
	}
	size_t len = REAC_FRAME_BYTES;        /* master frames are downstream width */
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	out[18] = 0x04; out[19] = 0x03;       /* the record container */
	out[20] = 0x00; out[21] = 0x13;       /* BE len 0x0013 */
	out[22] = 0x00; out[23] = 0x02;
	out[24] = 0x00; out[25] = 0xfe;
	out[26] = 0x13 - 5;                   /* preamble length echo: oplen - 5 */
	out[27] = 0xf0; out[28] = 0x41; out[29] = 0x0a;   /* f0 41 0a 00 00 */
	out[32] = 0x12; out[33] = 0x12;       /* record marker */
	out[34] = 0x01; out[35] = 0x01;       /* TAG 01 01 = head-amp */
	out[36] = ch; out[37] = param; out[38] = value;
	/* INNER record checksum: TAG..CKSUM sums to 0x80 mod 256. (For this
	 * record that reduces to CH+PARAM+VALUE+CKSUM == 0x7e, but compute the
	 * general record sum — the rule is the record's, not the head-amp's.) */
	unsigned s = 0;
	for (int i = 34; i < 39; i++)
		s += out[i];
	out[39] = (uint8_t)((0x80 - s) & 0xff);
	out[40] = 0xf7;                       /* record terminator */
	reac_ctrl_checksum_apply(out);        /* OUTER block checksum at [49] */
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
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
