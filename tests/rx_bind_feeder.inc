// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The slave side's receive gate, through the real feeder (src/reac_rx_bind.c), included by
 * test_reac_rx_gate.c, which owns the pcap writers and CHK. The pcap holds a desk and a
 * second host broadcasting valid 1492 B REAC frames beside it (its own MAC, its own counter).
 *
 *   control   unlocked (what the slave side did): the forger's frames are decoded, so the
 *             ring holds two sources' audio. This arm is what makes the next one mean
 *             something; if it stops decoding the forger the fixture is wrong.
 *   hint      locked at open from the hunt's MAC: only the desk is decoded, the forger is
 *             counted with the frames that are not ours; a learned MAC never moves it.
 *
 * These arms cross into libreac's feeder, which frames at libreac's own geometry, so they
 * live in the binary the copy finder already lists for that (tools/facts-perturb-check.sh);
 * the learned-MAC rule, which is reac-pw's alone, stays in test_reac_rx_bind.c. */

#include <reac/reac_macaddr.h>
#include "reac_rx_bind.h"

static const uint8_t BIND_DESK[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
static const uint8_t BIND_FORGER[6] = { 0x02, 0xde, 0xad, 0xbe, 0xef, 0x01 };

static void bind_mk_downstream(uint8_t *out, const uint8_t src[6], uint16_t counter, uint8_t level)
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

#define BIND_N 60

static void bind_write_pcap(char *path)
{
	int fd = mkstemp(path);
	CHK(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	pcap_hdr(f);
	uint8_t fr[REAC_FRAME_BYTES];
	for (uint16_t i = 0; i < BIND_N; i++) {
		bind_mk_downstream(fr, BIND_DESK, i, 0x40);
		pcap_rec(f, fr, sizeof fr);
		/* the forger counts from far away, so it is a different stream and not a
		 * stale copy of the desk's: the counter guard cannot be what drops it */
		bind_mk_downstream(fr, BIND_FORGER, (uint16_t)(30000 + i), 0x20);
		pcap_rec(f, fr, sizeof fr);
	}
	fclose(f);
}

static void bind_run_to_end(struct reac_rx *rx)
{
	CHK(reac_rx_start(rx) == 0);
	for (int i = 0; i < 3000; i++) {
		uint64_t seen = atomic_load(&rx->frames_ok) + atomic_load(&rx->frames_other) +
		                atomic_load(&rx->frames_dup) + atomic_load(&rx->frames_bad);
		if (seen >= 2 * BIND_N - 2)
			break;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(rx);
}

/* Drain the ring and say which sources' audio it holds: the desk writes +0.5 on channel 0,
 * the forger +0.25. This is what reaches the graph, which is the thing under test. */
static void bind_ring_sources(struct reac_ring *ring, int *desk, int *forger)
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

static int bind_open_rx(struct reac_rx *rx, struct reac_ring *ring, const char *path)
{
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
	                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
	return reac_rx_open(rx, &cfg, ring);
}

static void rx_bind_feeder_arms(void)
{
	char path[] = "/tmp/reacpw-bind-XXXXXX";
	bind_write_pcap(path);

	/* ---- control: unlocked, the forger is decoded beside the desk ---- */
	{
		struct reac_rx rx; struct reac_ring ring;
		CHK(bind_open_rx(&rx, &ring, path) == 0);
		bind_run_to_end(&rx);
		int d, f;
		bind_ring_sources(&ring, &d, &f);
		CHK(f > 0);                    /* the forger's audio reached the ring */
		reac_rx_close(&rx); reac_ring_free(&ring);
	}

	/* ---- hint: the hunt's MAC locks the gate; only the desk is decoded ---- */
	{
		struct reac_rx rx; struct reac_ring ring; struct reac_rx_bind b;
		CHK(bind_open_rx(&rx, &ring, path) == 0);
		reac_rx_bind_init(&b);
		reac_rx_bind_hint(&b, &rx, BIND_DESK);
		CHK(rx.up_src_locked == 1 && memcmp(rx.up_src, BIND_DESK, 6) == 0);
		bind_run_to_end(&rx);
		/* the pcap loops, so counts are not bounded by BIND_N: the forger is half the wire,
		 * so what is refused must be about as much as what is decoded */
		uint64_t ok = atomic_load(&rx.frames_ok), other = atomic_load(&rx.frames_other);
		CHK(ok >= BIND_N - 2);
		CHK(other >= BIND_N - 2 && other * 2 >= ok);   /* the forger, counted not decoded */
		int d, f;
		bind_ring_sources(&ring, &d, &f);
		CHK(d > 0 && f == 0);          /* only the desk's audio reached the ring */
		CHK(atomic_load(&rx.frames_bad) == 0);
		/* a learned MAC never moves a hunt hint, however long it is held */
		uint64_t forged48 = reac_mac48_pack(BIND_FORGER);
		for (uint64_t t = 0; t < 5; t++)
			CHK(reac_rx_bind_tick(&b, &rx, forged48, 1000, t * REAC_RX_BIND_STABLE_NS) == 0);
		CHK(memcmp(rx.up_src, BIND_DESK, 6) == 0);
		reac_rx_close(&rx); reac_ring_free(&ring);
	}

	unlink(path);
}
