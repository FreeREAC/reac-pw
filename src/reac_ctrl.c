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
		if (out->op0 == 0x04 && out->op1 == 0x03)
			out->kind = REAC_CTRL_GRANT;
		else if (out->op0 == 0x01 && out->op1 == 0x03 && out->op_len == 0x0019)
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

	/* Unicast-to-us box heartbeat: sel 0x81 keep-alive, sel 0x00 disconnect. */
	if (out->kind == REAC_CTRL_BOX_HB) {
		*ev = (out->sel == 0x00) ? REAC_M_RX_BOX_BYE : REAC_M_RX_BOX_UNICAST;
		return 0;
	}
	/* Any other unicast-to-us box frame — upstream FILLER (628/340 B),
	 * config-announce sel 0x82, unknown ctrl — proves the box linked to us. */
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
                              const uint8_t src[6], uint16_t counter)
{
	const int n_ch = 16;                 /* box width (S-1608-class 628 B) */
	size_t len = box_frame_len(n_ch);
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

/* ---- RECONSTRUCTED JOIN builders (experimental, not byte-verified) ---- */

size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch)
{
	size_t len = box_frame_len(16);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	out[18] = 0x01; out[19] = 0x03;
	out[20] = 0x00; out[21] = (uint8_t)in_ch;   /* BE len = our input channel count */
	out[22] = 0x82;                              /* master-reply selector */
	/* channel entries (3B: ch#, flags, 0xfe term) — layout reconstructed */
	for (int c = 0; c < in_ch && (24 + 3 * c + 2) < REAC_CTRL_CKSUM_OFF; c++) {
		out[24 + 3 * c] = (uint8_t)c;
		out[24 + 3 * c + 1] = 0x28;
		out[24 + 3 * c + 2] = 0xfe;
	}
	reac_ctrl_checksum_apply(out);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}

size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns)
{
	/* The BYTE-VERIFIED S-1608 cold-connect block (zoneA-48k capture): cdea 04 03,
	 * BE len 0x0014, then 00 02 00 fe + a fixed device descriptor. The 0x41 at
	 * block[10] is descriptor DATA, not a MAC tail (the earlier reconstruction
	 * wrote src[5] there — wrong: the block is MAC-independent; the master learns
	 * the box from the L2 source). Sum(block) mod 256 == 0 holds as captured. */
	static const uint8_t COLDCONNECT_BLK[32] = {
		0x04, 0x03, 0x00, 0x14, 0x00, 0x02, 0x00, 0xfe,
		0x0f, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
		0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x78, 0xf7,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	if (n_ch < 2 || n_ch > REAC_MAX_CHANNELS || (n_ch & 1))
		return 0;
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	memcpy(out + REAC_CTRL_BLOCK_OFF, COLDCONNECT_BLK, 32);
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
