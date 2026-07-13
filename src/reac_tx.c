// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_tx.h"

#include <reac/reac.h>      /* REAC_FRAME_BYTES, _AUDIO_OFFSET, _HDR_COUNTER_OFF, ... */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */

/* REAC_TX_LAYOUT — A/B override of the downstream audio byte layout, for the
 * Stage B re-listen protocol (docs/VALIDATION-PLAN.md). Default is the BRAID.
 *
 * The braid is the REAC wire format, confirmed by three independent sources
 * plus our own goldens:
 *   - reacdriver (per-gron, the macOS REAC driver): its to-device conversion
 *     is a 16-bit word byte-swap of big-endian s24 host PCM — out = in[1],
 *     in[0],in[3],in[2],in[5],in[4] per channel pair (MbufUtils.cpp), which is
 *     byte-identical to this braid ("LE bytes swapped");
 *   - obs-h8819 (norihiro): convert_to_pcm24lep, developed and LISTENING-
 *     validated against a real Roland M-200i downstream at 48 kHz — our exact
 *     console generation;
 *   - our own rig: the S-1608/S-0808 upstream return is this same braid,
 *     validated with real microphones (#108);
 *   - goldens: zoneA/zoneB (a real M-5000's two REAC ports, program audio)
 *     decode at coherence 0.99 / spectral flatness 0.002 under the braid at
 *     audio offset exactly 50, and as noise under every other layout x offset
 *     (docs/VALIDATION-PLAN.md Stage B coherence table).
 *
 * "plain" is the reac-aes67 reac_decode layout — the M-5000-generation claim
 * (e2e82ac: "plain LE coherent 0.999 on a live M-5000"). It is kept as the
 * A/B diagnostic and as the #135 per-generation candidate, but NOTE: the
 * zoneA/zoneB goldens (the same M-5000's two REAC ports, program audio)
 * CONTEST that claim — they decode braided, and the plain "coherence" is
 * explained by the mid-byte lane shift amplifying quiet braided audio 256x
 * into a coherent-looking image. On a de-braiding box, plain encode plays
 * every output as a ~-42 dBFS hash of its own mid/hi bytes — the exact
 * "right level, garbage content" complaint. #135 should re-validate a real
 * M-5000 with LOUD program before keying the encode per mixer profile. */
int reac_tx_layout_parse(const char *name)
{
	if (!name || !*name || !strcmp(name, "braid")) return REAC_TXL_BRAID;
	if (!strcmp(name, "plain")) return REAC_TXL_PLAIN;
	return -1;
}

/* Byte positions (lo,mid,hi) of sample s / channel ch in the 1440 B audio
 * region under `layout`. Both layouts are bijections over all 1440 bytes
 * (asserted by tests/test_reac_tx.c). */
void reac_tx_layout_pos(int layout, int s, int ch, size_t pos[3])
{
	if (layout == REAC_TXL_PLAIN) {
		size_t o = (size_t)(s * REAC_MAX_CHANNELS + ch) * REAC_RESOLUTION;
		pos[0] = o; pos[1] = o + 1; pos[2] = o + 2;
		return;
	}
	/* BRAID: each channel PAIR (2k, 2k+1) shares a 6-byte group at
	 * (s*40 + 2k)*3; even s24-LE (lo,mid,hi) -> g[3],g[0],g[1]; odd ->
	 * g[4],g[5],g[2]. Equivalently: 16-bit-word byte-swap of the pair packed
	 * as big-endian s24 (reacdriver's to-device conversion). */
	size_t g = (size_t)(s * REAC_MAX_CHANNELS + (ch & ~1)) * REAC_RESOLUTION;
	if ((ch & 1) == 0) {
		pos[0] = g + 3; pos[1] = g + 0; pos[2] = g + 1;
	} else {
		pos[0] = g + 4; pos[1] = g + 5; pos[2] = g + 2;
	}
}

static int tx_layout(void)
{
	static int l = -1;
	if (l < 0) {
		const char *e = getenv("REAC_TX_LAYOUT");
		int v = reac_tx_layout_parse(e);
		if (v < 0) {
			fprintf(stderr, "reac_tx: unknown REAC_TX_LAYOUT '%s', using braid\n", e);
			v = REAC_TXL_BRAID;
		} else if (v != REAC_TXL_BRAID) {
			fprintf(stderr, "reac_tx: DIAGNOSTIC downstream layout '%s' — A/B "
			        "listen test only (docs/VALIDATION-PLAN.md Stage B)\n", e);
		}
		l = v;
	}
	return l;
}

/* normalized float [-1,1) -> 24-bit signed LE (lo,mid,hi at p[0],p[1],p[2]), the
 * exact inverse of reac_rx.c's s24le_to_f32 (which divides by 2^23), so an
 * encode->decode round-trip is the identity up to one ULP of 24-bit
 * quantization. */
static inline void f32_to_s24le(float v, uint8_t *p)
{
	float x = v * 8388608.0f;            /* 2^23 */
	if (x > 8388607.0f) x = 8388607.0f;  /* clamp to the 24-bit signed range */
	if (x < -8388608.0f) x = -8388608.0f;
	int32_t s = (int32_t)lrintf(x);
	p[0] = (uint8_t)(s & 0xFF);          /* lo  */
	p[1] = (uint8_t)((s >> 8) & 0xFF);   /* mid */
	p[2] = (uint8_t)((s >> 16) & 0xFF);  /* hi  */
}

/* Standard Ethernet CRC-32 (IEEE 802.3), bit-reflected table-free form — see the
 * doc comment on the declaration in reac_tx.h for why this exists and why it is
 * NOT part of the encode path. */
uint32_t reac_eth_crc32(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
	}
	return ~crc;
}

int reac_tx_build(uint8_t *out, float *const *planar, int nch, int ns,
                  uint16_t counter, const uint8_t src[6])
{
	memset(out, 0, REAC_FRAME_BYTES);

	/* L2: broadcast dst (downstream master), our src, EtherType 0x8819. */
	memset(out, 0xFF, 6);
	memcpy(out + 6, src, 6);
	out[12] = (REAC_ETHERTYPE >> 8) & 0xFF;   /* 0x88 */
	out[13] = REAC_ETHERTYPE & 0xFF;          /* 0x19 */

	/* u16-LE sequence counter at byte 14; type 0x0000 (FILLER carries audio and
	 * is checksum-exempt) + the 32-byte control block at 18..49 stay zero. */
	out[REAC_HDR_COUNTER_OFF]     = (uint8_t)(counter & 0xFF);
	out[REAC_HDR_COUNTER_OFF + 1] = (uint8_t)((counter >> 8) & 0xFF);

	/* Audio: the REAC braid — the obs-h8819 even/odd channel-pair layout, the
	 * same braid as the box's upstream return (#108) and byte-identical to
	 * reacdriver's to-device 16-bit-word swap of BE s24. Audio starts at byte 50
	 * exactly (golden offset scan: any other offset decodes real desk program as
	 * noise). See the REAC_TX_LAYOUT block above for the full evidence trail.
	 * Channels beyond nch (or NULL planes) are silent.
	 *
	 * History: this encode was braid (895afb9, correct), then regressed to
	 * plain-LE when a full-scale clipped program pile + a box-in1->out8 loopback
	 * on playback_08 was misattributed to the braid as a "burst" — the plain
	 * encode only ATTENUATED the garbage to a -42 dBFS hash ("right level,
	 * garbage content"), it did not fix anything. reac_tx_layout_pos() keeps the
	 * plain variant available for the A/B listen test. */
	uint8_t *audio = out + REAC_AUDIO_OFFSET;
	const int N = REAC_MAX_CHANNELS;
	const int l = tx_layout();
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++) {
		for (int ch = 0; ch < N; ch++) {
			float v = (ch < nch && planar[ch]) ? planar[ch][s] : 0.0f;
			uint8_t b[3];
			size_t pos[3];
			f32_to_s24le(v, b);
			reac_tx_layout_pos(l, s, ch, pos);
			audio[pos[0]] = b[0];
			audio[pos[1]] = b[1];
			audio[pos[2]] = b[2];
		}
	}

	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;  /* 0xC2 */
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;  /* 0xEA */
	return REAC_FRAME_BYTES;
}

int reac_tx_open(struct reac_tx *tx, const char *ifname)
{
	tx->fd = -1;
	tx->ifindex = 0;
	tx->counter = 0;
	/* Stand-in source MAC: Roland OUI 00:40:ab + a fixed host part. The decoder
	 * ignores src, and this keeps emitted frames sanitization-clean (no rig MAC). */
	static const uint8_t standin[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };
	memcpy(tx->src, standin, 6);

	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	tx->ifindex = ifr.ifr_ifindex;
	tx->fd = fd;
	return 0;
}

void reac_tx_close(struct reac_tx *tx)
{
	if (tx->fd >= 0)
		close(tx->fd);
	tx->fd = -1;
}

int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns)
{
	uint8_t frame[REAC_FRAME_BYTES];
	reac_tx_build(frame, planar, nch, ns, tx->counter, tx->src);

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family  = AF_PACKET;
	sll.sll_ifindex = tx->ifindex;
	sll.sll_halen   = 6;
	memset(sll.sll_addr, 0xFF, 6);  /* broadcast dst */

	ssize_t r = sendto(tx->fd, frame, REAC_FRAME_BYTES, 0,
	                   (struct sockaddr *)&sll, sizeof sll);
	tx->counter++;  /* free-running, wraps at 16 bits like the desk's */
	return (int)r;
}
