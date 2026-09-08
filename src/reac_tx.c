// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_tx.h"

#include <reac/reac.h>         /* REAC_FRAME_BYTES, REAC_ETHERTYPE */
#include <reac/reac_encode.h>  /* reac_downstream_build — the frame builder */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */

/* The downstream frame's byte layout is not stated here any more. It moved to
 * libreac with the encoder on 2026-07-29: <reac/reac_encode.h> (the builder and
 * its full rationale) over <reac/reac_braid.h> (the byte map, with the
 * reacdriver / obs-h8819 / rig #108 / zoneA-zoneB M-5000 evidence trail and the
 * debunking of the historical "plain" alternative). One home for the wire
 * format, in both directions — do not restate it in this file. */

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

int reac_tx_emit_frame(struct reac_tx *tx, const uint8_t *frame, size_t len)
{
	if (!tx || tx->fd < 0 || !frame || len == 0)
		return -1;
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family  = AF_PACKET;
	sll.sll_ifindex = tx->ifindex;
	sll.sll_halen   = 6;
	memset(sll.sll_addr, 0xFF, 6);  /* broadcast dst */

	return (int)sendto(tx->fd, frame, len, 0, (struct sockaddr *)&sll, sizeof sll);
}

int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns)
{
	uint8_t frame[REAC_FRAME_BYTES];
	reac_downstream_build(frame, planar, nch, ns, tx->counter, tx->src);
	int r = reac_tx_emit_frame(tx, frame, REAC_FRAME_BYTES);
	tx->counter++;  /* free-running, wraps at 16 bits like the desk's */
	return r;
}
