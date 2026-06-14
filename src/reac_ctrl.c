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

/* box-width frame length for n_ch inputs */
static size_t box_frame_len(int n_ch)
{
	return (size_t)AUDIO_OFF + (size_t)n_ch * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION + 2;
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
	if (n_ch < 1 || n_ch > REAC_MAX_CHANNELS)
		return 0;
	size_t len = box_frame_len(n_ch);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0x00, 0x00);   /* unicast FILLER */
	/* 32-byte descriptor [18:50] = 00 7a per slot (16 slots), as the real box. */
	for (int k = 0; k < 16; k++) {
		out[18 + 2 * k] = DESC_WORD_HI;
		out[18 + 2 * k + 1] = DESC_WORD_LO;
	}
	/* audio [50:..] plain-LE sample-major (s*n_ch+ch)*3. Slot placement is
	 * positional (the FPGA scramble of a real box is unresolved, task #61). */
	uint8_t *audio = out + AUDIO_OFF;
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++)
		for (int ch = 0; ch < n_ch; ch++) {
			float v = planar && planar[ch] ? planar[ch][s] : 0.0f;
			f32_to_s24le(v, audio + (size_t)(s * n_ch + ch) * REAC_RESOLUTION);
		}
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;                            /* FILLER: no checksum (exempt) */
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
                                   const uint8_t src[6], uint16_t counter)
{
	size_t len = box_frame_len(16);
	memset(out, 0, len);
	put_hdr(out, master, src, counter, 0xcd, 0xea);
	out[18] = 0x04; out[19] = 0x03;       /* cdea 04 03 — sub-cmd 04 = connect */
	out[20] = 0x00; out[21] = 0x14;       /* BE len 0x0014 (or 0x0013) */
	out[22] = 0x02;                       /* selector */
	out[23] = 0x00; out[24] = 0xfe;       /* payload prefix (§13b: 0002 00 fe ...) */
	/* tail carries our src-MAC low byte (the §13b "41" = box MAC tail) */
	out[27] = src[5];
	reac_ctrl_checksum_apply(out);
	out[len - 2] = REAC_END_MARKER_0; out[len - 1] = REAC_END_MARKER_1;
	return len;
}
