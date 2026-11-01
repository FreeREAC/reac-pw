// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the slave side's receive gate is locked to the desk it slaves to.
 *
 * The real feeder thread runs over a pcap in which a second host broadcasts 1492 B REAC
 * frames beside the desk (same length, valid, its own MAC, its own counter), and the lock
 * is applied by reac_rx_bind_hint / reac_rx_bind_tick, the two calls main.c makes.
 *
 *   control   unlocked (what the slave side did): the forger's frames are decoded, so the
 *             ring holds two sources' audio. This arm is what makes the others mean
 *             something; if it stops decoding the forger the fixture is wrong.
 *   hint      locked at open from the hunt's MAC: only the desk is decoded, the forger is
 *             counted with the frames that are not ours.
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
#include <reac/reac_braid.h>
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

static void pcap_hdr(FILE *f)
{
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
}
static void pcap_rec(FILE *f, const uint8_t *frame, uint32_t len)
{
	uint32_t rh[4] = { 0, 0, len, len };
	fwrite(rh, sizeof rh, 1, f);
	fwrite(frame, len, 1, f);
}

static void mk_downstream(uint8_t *out, const uint8_t src[6], uint16_t counter, uint8_t level)
{
	memset(out, 0, REAC_FRAME_BYTES);
	memset(out, 0xff, 6);
	memcpy(out + 6, src, 6);
	out[REAC_ETHERTYPE_OFF] = REAC_ETHERTYPE >> 8;
	out[REAC_ETHERTYPE_OFF + 1] = REAC_ETHERTYPE & 0xff;
	out[REAC_HDR_COUNTER_OFF] = (uint8_t)(counter & 0xff);
	out[REAC_HDR_COUNTER_OFF + 1] = (uint8_t)(counter >> 8);
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, 0, REAC_MAX_CHANNELS, pos);
		uint8_t *audio = out + REAC_L2_HEADER_LEN;
		audio[pos[0]] = 0x00; audio[pos[1]] = 0x00; audio[pos[2]] = level;   /* s24 0x400000 = +0.5, 0x200000 = +0.25 */
	}
	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

#define N 60

static void write_pcap(char *path)
{
	int fd = mkstemp(path);
	CHK(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	pcap_hdr(f);
	uint8_t fr[REAC_FRAME_BYTES];
	for (uint16_t i = 0; i < N; i++) {
		mk_downstream(fr, DESK, i, 0x40);
		pcap_rec(f, fr, sizeof fr);
		/* the forger counts from far away, so it is a different stream and not a
		 * stale copy of the desk's: the counter guard cannot be what drops it */
		mk_downstream(fr, FORGER, (uint16_t)(30000 + i), 0x20);
		pcap_rec(f, fr, sizeof fr);
	}
	fclose(f);
}

static void run_to_end(struct reac_rx *rx)
{
	CHK(reac_rx_start(rx) == 0);
	for (int i = 0; i < 3000; i++) {
		uint64_t seen = atomic_load(&rx->frames_ok) + atomic_load(&rx->frames_other) +
		                atomic_load(&rx->frames_dup) + atomic_load(&rx->frames_bad);
		if (seen >= 2 * N - 2)
			break;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(rx);
}

/* Drain the ring and say which sources' audio it holds: the desk writes +0.5 on channel 0,
 * the forger +0.25. This is what reaches the graph, which is the thing under test. */
static void ring_sources(struct reac_ring *ring, int *desk, int *forger)
{
	float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *dst[REAC_MAX_CHANNELS];
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) dst[c] = ch[c];
	*desk = *forger = 0;
	for (int i = 0; i < 100000; i++) {
		if (reac_ring_read_planar(ring, dst, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT) !=
		    REAC_SAMPLES_PER_PKT)
			break;
		if (ch[0][0] > 0.49f && ch[0][0] < 0.51f) (*desk)++;
		if (ch[0][0] > 0.24f && ch[0][0] < 0.26f) (*forger)++;
	}
}

static int open_rx(struct reac_rx *rx, struct reac_ring *ring, const char *path)
{
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
	                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
	return reac_rx_open(rx, &cfg, ring);
}

int main(void)
{
	char path[] = "/tmp/reacpw-bind-XXXXXX";
	write_pcap(path);

	/* the bar restated here must be libreac's, where libreac has one */
#ifdef REAC_HUNT_RIVAL_FRAMES
	CHK(REAC_RX_BIND_FRAMES == REAC_HUNT_RIVAL_FRAMES);
#endif

	/* ---- control: unlocked, the forger is decoded beside the desk ---- */
	{
		struct reac_rx rx; struct reac_ring ring;
		CHK(open_rx(&rx, &ring, path) == 0);
		run_to_end(&rx);
		int d, f;
		ring_sources(&ring, &d, &f);
		CHK(f > 0);                    /* the forger's audio reached the ring */
		reac_rx_close(&rx); reac_ring_free(&ring);
	}

	/* ---- hint: the hunt's MAC locks the gate; only the desk is decoded ---- */
	{
		struct reac_rx rx; struct reac_ring ring; struct reac_rx_bind b;
		CHK(open_rx(&rx, &ring, path) == 0);
		reac_rx_bind_init(&b);
		reac_rx_bind_hint(&b, &rx, DESK);
		CHK(rx.up_src_locked == 1 && memcmp(rx.up_src, DESK, 6) == 0);
		run_to_end(&rx);
		/* the pcap loops, so counts are not bounded by N: the forger is half the wire,
		 * so what is refused must be about as much as what is decoded */
		uint64_t ok = atomic_load(&rx.frames_ok), other = atomic_load(&rx.frames_other);
		CHK(ok >= N - 2);
		CHK(other >= N - 2 && other * 2 >= ok);   /* the forger, counted not decoded */
		int d, f;
		ring_sources(&ring, &d, &f);
		CHK(d > 0 && f == 0);          /* only the desk's audio reached the ring */
		CHK(atomic_load(&rx.frames_bad) == 0);
		/* a learned MAC never moves a hunt hint, however long it is held */
		uint64_t forged48 = reac_mac48_pack(FORGER);
		for (uint64_t t = 0; t < 5; t++)
			CHK(reac_rx_bind_tick(&b, &rx, forged48, 1000, t * REAC_RX_BIND_STABLE_NS) == 0);
		CHK(memcmp(rx.up_src, DESK, 6) == 0);
		reac_rx_close(&rx); reac_ring_free(&ring);
	}

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
