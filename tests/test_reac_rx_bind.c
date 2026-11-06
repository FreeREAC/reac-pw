// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the slave side's receive gate is locked to the desk it slaves to.
 *
 * The lock is applied by reac_rx_bind_hint / reac_rx_bind_tick, the two calls main.c makes.
 * The arms that run the real feeder over a pcap with a forger beside the desk (control,
 * hint) are in tests/rx_bind_feeder.inc, run by test_reac_rx_gate; this binary holds the
 * rule that is reac-pw's alone:
 *
 *   learned   the slave engine's master MAC binds only after the streaming bar and a
 *             stable window; one frame, or a MAC that flaps, binds nothing; a different
 *             MAC held for the window moves the lock; a hunt hint is never moved by it.
 * No sockets, no PipeWire. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include <reac/reac.h>
#include <reac/reac_macaddr.h>
#include <reac/reac_hunt.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

#include "reac_facts_pw.h"   /* the protocol numbers, from their one declaration */
#include "reac_rx_bind.h"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const uint8_t DESK[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
static const uint8_t FORGER[6] = { 0x02, 0xde, 0xad, 0xbe, 0xef, 0x01 };
static const uint8_t DESK2[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x91 };

/* An rx opened over an empty capture: the learned-MAC rule reads and writes only the
 * gate's lock, so no frame has to be fed for it. */
static int open_rx(struct reac_rx *rx, struct reac_ring *ring, const char *path)
{
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
	                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
	return reac_rx_open(rx, &cfg, ring);
}

static void write_empty_pcap(char *path)
{
	int fd = mkstemp(path);
	CHK(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
	fclose(f);
}

int main(void)
{
	char path[] = "/tmp/reacpw-bind-XXXXXX";
	write_empty_pcap(path);

	/* the bar restated here must be libreac's, where libreac has one */
#ifdef REAC_HUNT_RIVAL_FRAMES
	CHK(REAC_RX_BIND_FRAMES == REAC_HUNT_RIVAL_FRAMES);
#endif

	/* ---- learned: one frame decides nothing ---- */
	{
		struct reac_rx rx; struct reac_ring ring; struct reac_rx_bind b;
		CHK(open_rx(&rx, &ring, path) == 0);
		reac_rx_bind_init(&b);
		const uint64_t D = reac_mac48_pack(DESK), F = reac_mac48_pack(FORGER);
		const uint64_t W = REAC_RX_BIND_STABLE_NS;
		/* first sight starts a clock, it locks nothing */
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, 0) == 0 && rx.up_src_locked == 0);
		/* held for the window, but the gate has not decoded the streaming bar */
		CHK(reac_rx_bind_tick(&b, &rx, D, REAC_RX_BIND_FRAMES - 1, 2 * W) == 0);
		CHK(rx.up_src_locked == 0);
		/* held for less than the window */
		reac_rx_bind_init(&b);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, 0) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, W - 1) == 0 && rx.up_src_locked == 0);
		/* a flapping answer never completes a window */
		reac_rx_bind_init(&b);
		for (int i = 0; i < 20; i++)
			CHK(reac_rx_bind_tick(&b, &rx, (i & 1) ? D : F, 1000, (uint64_t)i * (W / 2)) == 0);
		CHK(rx.up_src_locked == 0);
		/* a vanished answer drops the clock */
		reac_rx_bind_init(&b);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, 0) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, 0, 1000, W / 2) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, W) == 0);       /* restarted at W */
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, W + W - 1) == 0 && rx.up_src_locked == 0);
		/* held for the window with the bar met: locked, once */
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, W + W) == 1);
		CHK(rx.up_src_locked == 1 && memcmp(rx.up_src, DESK, 6) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, 4 * W) == 0);   /* idempotent */
		/* a single different answer moves nothing... */
		CHK(reac_rx_bind_tick(&b, &rx, F, 1000, 5 * W) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, D, 1000, 5 * W + 1) == 0);
		CHK(memcmp(rx.up_src, DESK, 6) == 0);
		/* ...but a desk replaced behind the port, held for the window, is followed */
		const uint64_t D2 = reac_mac48_pack(DESK2);
		CHK(reac_rx_bind_tick(&b, &rx, D2, 1000, 10 * W) == 0);
		CHK(memcmp(rx.up_src, DESK, 6) == 0);
		CHK(reac_rx_bind_tick(&b, &rx, D2, 1000, 11 * W) == 1);
		CHK(memcmp(rx.up_src, DESK2, 6) == 0);
		reac_rx_close(&rx); reac_ring_free(&ring);
	}

	unlink(path);
	if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
	printf("reac_rx_bind: ok\n");
	return 0;
}
