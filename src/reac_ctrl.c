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
