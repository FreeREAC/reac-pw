// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_rx_bind.h"

#include <reac/reac_macaddr.h>
#include <reac/transport/reac_rx.h>

#include <string.h>

void reac_rx_bind_init(struct reac_rx_bind *b)
{
	if (b)
		memset(b, 0, sizeof *b);
}

static void lock_to(struct reac_rx_bind *b, struct reac_rx *rx, const uint8_t mac[6],
                    int from_hint)
{
	memcpy(b->locked_mac, mac, 6);
	b->locked = 1;
	b->from_hint = from_hint;
	reac_rx_peer_reset(rx, mac, REAC_RX_BIND_SESSION);
}

void reac_rx_bind_hint(struct reac_rx_bind *b, struct reac_rx *rx, const uint8_t mac[6])
{
	if (!b || !rx || !mac)
		return;
	lock_to(b, rx, mac, 1);
}

int reac_rx_bind_tick(struct reac_rx_bind *b, struct reac_rx *rx,
                      uint64_t learned_mac48, uint64_t frames_ok, uint64_t now_ns)
{
	if (!b || !rx)
		return 0;
	/* The hunt's answer stands: a streamed rival, decided once. */
	if (b->locked && b->from_hint)
		return 0;
	if (learned_mac48 == 0) {
		b->have_cand = 0;
		return 0;
	}
	uint8_t mac[6];
	reac_mac48_unpack(learned_mac48, mac);
	if (b->locked && memcmp(b->locked_mac, mac, 6) == 0) {
		b->have_cand = 0;
		return 0;
	}
	/* Time this MAC. A different one restarts the clock: held for the whole window
	 * or not at all. */
	if (!b->have_cand || memcmp(b->cand, mac, 6) != 0) {
		memcpy(b->cand, mac, 6);
		b->have_cand = 1;
		b->cand_since_ns = now_ns;
		return 0;
	}
	if (frames_ok < REAC_RX_BIND_FRAMES || now_ns < b->cand_since_ns ||
	    now_ns - b->cand_since_ns < REAC_RX_BIND_STABLE_NS)
		return 0;
	lock_to(b, rx, mac, 0);
	b->have_cand = 0;
	return 1;
}
