// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_tx.h"

#include <reac/reac.h>      /* REAC_FRAME_BYTES, _AUDIO_OFFSET, _HDR_COUNTER_OFF, ... */
#include <reac/reac_braid.h>   /* reac_braid_pos — the layout oracle */
#include <reac/reac_sample.h>  /* reac_f32_to_s24le */

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

/* The downstream audio byte layout is the BRAID — the REAC wire format. The
 * byte map and its full evidence trail (reacdriver / obs-h8819 / rig #108 /
 * zoneA-zoneB M-5000 goldens, plus the debunking of the historical "plain"
 * alternative — the -42 dBFS mid-byte hash reproduced as the negative control
 * in tests/test_reac_tx.c) live with the oracle: <reac/reac_braid.h>. The
 * REAC_TX_LAYOUT A/B env override that kept plain selectable was removed once
 * the Stage B listen test confirmed the braid (docs/VALIDATION-PLAN.md). This
 * encoder takes byte positions from reac_braid_pos() and the s24 conversion
 * from <reac/reac_sample.h> — no local copy of either. */

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
	 * noise). See the braid evidence block above for the full trail.
	 * Channels beyond nch (or NULL planes) are silent.
	 *
	 * History: this encode was braid (895afb9, correct), then regressed to
	 * plain-LE when a full-scale clipped program pile + a box-in1->out8 loopback
	 * on playback_08 was misattributed to the braid as a "burst" — the plain
	 * encode only ATTENUATED the garbage to a -42 dBFS hash ("right level,
	 * garbage content"), it did not fix anything. */
	uint8_t *audio = out + REAC_AUDIO_OFFSET;
	const int N = REAC_MAX_CHANNELS;
	int frames = ns < REAC_SAMPLES_PER_PKT ? ns : REAC_SAMPLES_PER_PKT;
	for (int s = 0; s < frames; s++) {
		for (int ch = 0; ch < N; ch++) {
			float v = (ch < nch && planar[ch]) ? planar[ch][s] : 0.0f;
			uint8_t b[3];
			size_t pos[3];
			reac_f32_to_s24le(v, b);
			reac_braid_pos(s, ch, N, pos);
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
