// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the OHRCA duplicate-frame guard in the RX feeder (frames_dup).
 *
 * Regression pin for the granulated-audio bug (see reac_rx.h's prev_frame
 * contract and docs/OHRCA-UPSTREAM-DUPLICATE-FRAMES.md): a mirrored RX path
 * (or a box driven at the doubled OHRCA cadence) delivers every upstream
 * frame TWICE, byte-identically. Feeding both copies into the ring plays
 * every 12-sample block twice — a granular per-frame stutter — and doubles
 * the rate the ppm estimator sees. The guard drops the exact repeat before
 * the counter/ppm/decode path.
 *
 * Same harness pattern as test_reac_rx_gate.c: synthesize a temp pcap and
 * run the REAL reac_rx feeder thread over it. Frames derive from the UP16
 * captured S-1608 fixture with distinct counters (crossing the 0xffff -> 0
 * counter seam) and a distinct per-frame audio marker, so:
 *
 *   DOUBLED delivery (each frame twice, byte-identical, duplicates also
 *   spanning the counter seam): every accepted frame has exactly one dropped
 *   twin (frames_dup tracks frames_ok), and the ring carries the SINGLE
 *   cadence — each distinct frame's 12-sample block exactly once, in order.
 *
 *   NEGATIVE (a true doubled-cadence source): distinct frames back-to-back
 *   at twice the rate are never byte-identical -> frames_dup == 0 and every
 *   frame's audio reaches the ring.
 *
 * No sockets, no PipeWire — pcap replay flat-out, unit scope.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <reac/reac.h>
#include "reac_ring.h"
#include "reac_rx.h"
#include <reac/reac_upstream.h>

#include "upstream_fixtures.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- minimal classic-pcap writer (LE, linktype 1) ---- */
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

#define NFRAMES 40
/* counters start just below the wrap so the doubled pair straddles the seam */
#define CTR_BASE 0xffec

/* The i-th DISTINCT upstream frame: the UP16 capture with counter CTR_BASE+i
 * (crossing 0xffff -> 0x0000 inside the run) and one audio byte marked per
 * frame so every frame decodes to distinct PCM. */
static void mk_frame(uint8_t *out, int i)
{
	memcpy(out, UP16, sizeof UP16);
	uint16_t ctr = (uint16_t)(CTR_BASE + i);
	out[14] = (uint8_t)(ctr & 0xff);
	out[15] = (uint8_t)(ctr >> 8);
	/* distinct audio marker: a byte inside the braided audio region
	 * [REAC_L2_HEADER_LEN : 626) — which decoded sample it lands in is
	 * irrelevant, the expected PCM is recomputed per frame below */
	out[REAC_L2_HEADER_LEN + 7] = (uint8_t)(0x40 + i);
}

/* run the feeder until it has accepted want_ok frames (or ~2 s timeout) */
static int run_rx(struct reac_rx *rx, uint64_t want_ok)
{
	if (reac_rx_start(rx) != 0)
		return -1;
	for (int i = 0; i < 2000; i++) {
		if (atomic_load(&rx->frames_ok) >= want_ok)
			break;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(rx);
	return atomic_load(&rx->frames_ok) >= want_ok ? 0 : -1;
}

int main(void)
{
	uint8_t frame[sizeof UP16];

	/* the expected decoded PCM of every distinct frame, from the real decoder */
	static uint8_t pcm[NFRAMES][16 * REAC_SAMPLES_PER_PKT * 3];
	for (int i = 0; i < NFRAMES; i++) {
		mk_frame(frame, i);
		CHK(reac_upstream_decode(frame, sizeof frame, pcm[i]) == REAC_SAMPLES_PER_PKT);
	}

	/* ---- DOUBLED delivery: every frame twice, byte-identical ---- */
	{
		char path[] = "/tmp/reacpw-dup-XXXXXX";
		int fd = mkstemp(path);
		CHK(fd >= 0);
		FILE *f = fdopen(fd, "wb");
		CHK(f != NULL);
		pcap_hdr(f);
		for (int i = 0; i < NFRAMES; i++) {
			mk_frame(frame, i);
			pcap_rec(f, frame, sizeof frame);   /* the frame ... */
			pcap_rec(f, frame, sizeof frame);   /* ... and its mirror twin */
		}
		fclose(f);

		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = 48000, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_UPSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, NFRAMES) == 0);
		uint64_t ok  = atomic_load(&rx.frames_ok);
		uint64_t dup = atomic_load(&rx.frames_dup);
		CHK(ok >= NFRAMES);
		/* every accepted frame had exactly one byte-identical twin dropped:
		 * frames_dup == N per N accepted (the feeder may stop between a frame
		 * and its twin, hence the one-frame in-flight tolerance). Holds across
		 * the counter seam and the pcap-loop seam alike. */
		CHK(dup == ok || dup + 1 == ok);
		CHK(dup >= NFRAMES - 1);
		CHK(atomic_load(&rx.frames_bad) == 0);
		CHK(atomic_load(&rx.frames_other) == 0);
		/* no counter gaps: the dedup runs BEFORE the counter tracking, so the
		 * estimator saw the clean single-cadence 0xffec..0x0013 sequence (the
		 * doubled twins never registered as counter stalls) */
		CHK(atomic_load(&rx.counter_gaps) == 0);

		/* the ring carries the SINGLE cadence: the first NFRAMES 12-sample
		 * blocks are frames 0..NFRAMES-1 in order, each exactly once — a fed
		 * duplicate would double a block and shift the sequence */
		float ch[REAC_MAX_CHANNELS][NFRAMES * REAC_SAMPLES_PER_PKT];
		float *dst[REAC_MAX_CHANNELS];
		for (int c = 0; c < REAC_MAX_CHANNELS; c++) dst[c] = ch[c];
		CHK(reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS,
		                          NFRAMES * REAC_SAMPLES_PER_PKT)
		    == NFRAMES * REAC_SAMPLES_PER_PKT);
		int bad = 0;
		for (int i = 0; i < NFRAMES; i++)
			for (int c = 0; c < 16; c++)
				for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
					const uint8_t *p = &pcm[i][(size_t)(c * 12 + s) * 3];
					int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
					                      ((uint32_t)p[2] << 16));
					if (v & 0x00800000) v |= ~0x00FFFFFF;
					if (fabsf(ch[c][i * 12 + s] - (float)v / 8388608.0f) > 1e-7f)
						bad++;
				}
		CHK(bad == 0);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		unlink(path);
	}

	/* ---- NEGATIVE: distinct frames at the doubled cadence -> dup == 0 ---- */
	{
		char path[] = "/tmp/reacpw-dup-neg-XXXXXX";
		int fd = mkstemp(path);
		CHK(fd >= 0);
		FILE *f = fdopen(fd, "wb");
		CHK(f != NULL);
		pcap_hdr(f);
		/* 2N DISTINCT frames back-to-back — a genuine 96 kHz box: twice the
		 * frame rate but never a byte-identical repeat */
		for (int i = 0; i < 2 * NFRAMES; i++) {
			memcpy(frame, UP16, sizeof UP16);
			uint16_t ctr = (uint16_t)(CTR_BASE + i);
			frame[14] = (uint8_t)(ctr & 0xff);
			frame[15] = (uint8_t)(ctr >> 8);
			frame[REAC_L2_HEADER_LEN + 7] = (uint8_t)i;
			pcap_rec(f, frame, sizeof frame);
		}
		fclose(f);

		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = 96000, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_UPSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, 2 * NFRAMES) == 0);
		CHK(atomic_load(&rx.frames_ok) >= 2 * NFRAMES);
		CHK(atomic_load(&rx.frames_dup) == 0);   /* the guard is a strict no-op */
		CHK(atomic_load(&rx.frames_bad) == 0);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		unlink(path);
	}

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: rx dup guard — doubled byte-identical delivery drops exactly one twin "
	       "per frame (across the counter seam), the ring carries the single cadence "
	       "in order, and distinct frames at doubled cadence are untouched (dup == 0)\n");
	return 0;
}
