// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the duplicate-frame guard in the RX feeder (frames_dup).
 *
 * Regression pin for the granulated-audio bug (see reac_rx.h's prev_frame
 * contract and docs/OHRCA-UPSTREAM-DUPLICATE-FRAMES.md). The same frame
 * reaches the feeder twice from two unrelated causes, and both must be
 * dropped before the counter/ppm/decode path:
 *
 *   1. the OVER-CLOCK repeat — a box driven at the doubled cadence re-sends
 *      each frame verbatim, SAME LENGTH;
 *   2. the MIRROR TWIN — a rig mirroring both RX and TX of one port sees a
 *      transiting frame twice, one copy carrying 2 bytes of the frame's own
 *      Ethernet FCS after the C2 EA end marker. Those two copies differ in
 *      LENGTH (1492/1494 downstream, 628/630 upstream), so a raw-length
 *      compare never fires on the pair — the guard must compare
 *      reac_frame_clean_len() bytes (FreeREAC/reac-pw#82).
 *
 * Feeding both copies into the ring plays every 12-sample block twice — a
 * granular per-frame stutter — and doubles the rate the ppm estimator sees.
 *
 * Same harness pattern as test_reac_rx_gate.c: synthesize a temp pcap and
 * run the REAL reac_rx feeder thread over it. Cases:
 *
 *   DOUBLED delivery (UP16 upstream fixture, each frame twice, byte-identical,
 *   duplicates also spanning the 0xffff -> 0 counter seam): every accepted
 *   frame has exactly one dropped twin (frames_dup tracks frames_ok), and the
 *   ring carries the SINGLE cadence — each distinct frame's 12-sample block
 *   exactly once, in order.
 *
 *   MIRROR TWIN, upstream (628 B clean + 630 B carrying the real low-16 FCS):
 *   same result, on a pair the old same-length test could not see.
 *
 *   MIRROR TWIN, downstream (1492 B clean + 1494 B with the residue): the
 *   pair named in #82, through the DOWNSTREAM accept gate.
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
#include <reac/reac_braid.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>
#include <reac/reac_upstream.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

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

/* Ethernet FCS (CRC-32/ISO-HDLC) — used to build a FAITHFUL mirror twin: the
 * residue a mirrored capture leaves after the end marker is the low 16 bits of
 * the frame's own FCS, little-endian (measured over the whole capture corpus,
 * 217,558/217,558 frames). The guard does not read those bytes — that is the
 * point, it compares the clean prefix — but the fixture must be the real thing
 * rather than two arbitrary bytes. */
static uint32_t fcs32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

/* Append the 2-byte FCS residue, turning a clean frame into its mirror twin.
 * `out` must have room for len + 2. */
static void add_fcs_residue(uint8_t *out, size_t len)
{
	uint32_t fcs = fcs32(out, len);
	out[len]     = (uint8_t)(fcs & 0xff);
	out[len + 1] = (uint8_t)((fcs >> 8) & 0xff);
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

/* A well-formed synthetic 40-ch downstream broadcast frame, counter-stamped and
 * marked on channel 0 through the braid oracle (same fixture shape as
 * test_reac_rx_gate.c's). `out` must have room for REAC_FRAME_BYTES + 2 so the
 * mirror twin can be built in place. */
static void mk_downstream(uint8_t *out, uint16_t counter)
{
	memset(out, 0, REAC_FRAME_BYTES);
	memset(out, 0xff, 6);                       /* dst broadcast */
	static const uint8_t master[6] = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
	memcpy(out + 6, master, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[14] = (uint8_t)(counter & 0xff); out[15] = (uint8_t)(counter >> 8);
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, 0, 40, pos);
		uint8_t *audio = out + REAC_L2_HEADER_LEN;
		audio[pos[0]] = (uint8_t)counter;       /* per-frame marker: distinct PCM */
		audio[pos[1]] = 0x00;
		audio[pos[2]] = 0x40;
	}
	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
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
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
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
					const uint8_t *p = &pcm[i][(size_t)(c * REAC_SAMPLES_PER_PKT + s) * 3];
					int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
					                      ((uint32_t)p[2] << 16));
					if (v & 0x00800000) v |= ~0x00FFFFFF;
					if (fabsf(ch[c][i * REAC_SAMPLES_PER_PKT + s] - (float)v / 8388608.0f) > 1e-7f)
						bad++;
				}
		CHK(bad == 0);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		unlink(path);
	}

	/* ---- MIRROR TWIN, upstream: 628 B clean + 630 B with the FCS residue ----
	 * The pair the pre-#82 same-length test could not see: the two copies are
	 * byte-identical over the clean 628 B and differ only in the 2 bytes the
	 * capture path kept, so `n == prev_frame_len` is false for every twin. */
	{
		CHK(reac_frame_clean_len(sizeof UP16 + 2) == sizeof UP16);
		CHK(reac_frame_clean_len(sizeof UP16) == sizeof UP16);

		char path[] = "/tmp/reacpw-dup-twin-XXXXXX";
		int fd = mkstemp(path);
		CHK(fd >= 0);
		FILE *f = fdopen(fd, "wb");
		CHK(f != NULL);
		pcap_hdr(f);
		uint8_t twin[sizeof UP16 + 2];
		for (int i = 0; i < NFRAMES; i++) {
			mk_frame(frame, i);
			pcap_rec(f, frame, sizeof frame);       /* the clean copy ... */
			memcpy(twin, frame, sizeof frame);
			add_fcs_residue(twin, sizeof frame);
			pcap_rec(f, twin, sizeof twin);         /* ... and its mirror twin */
		}
		fclose(f);

		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_UPSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, NFRAMES) == 0);
		uint64_t ok  = atomic_load(&rx.frames_ok);
		uint64_t dup = atomic_load(&rx.frames_dup);
		CHK(ok >= NFRAMES);
		CHK(dup == ok || dup + 1 == ok);
		CHK(dup >= NFRAMES - 1);
		CHK(atomic_load(&rx.frames_bad) == 0);
		CHK(atomic_load(&rx.frames_other) == 0);
		CHK(atomic_load(&rx.counter_gaps) == 0);

		/* the ring carries the single cadence, in order — a fed twin would
		 * double a 12-sample block and shift everything after it */
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
					const uint8_t *p = &pcm[i][(size_t)(c * REAC_SAMPLES_PER_PKT + s) * 3];
					int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
					                      ((uint32_t)p[2] << 16));
					if (v & 0x00800000) v |= ~0x00FFFFFF;
					if (fabsf(ch[c][i * REAC_SAMPLES_PER_PKT + s] - (float)v / 8388608.0f) > 1e-7f)
						bad++;
				}
		CHK(bad == 0);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		unlink(path);
	}

	/* ---- MIRROR TWIN, downstream: 1492 B clean + 1494 B with the residue ----
	 * The pair #82 names. Through the DOWNSTREAM accept gate, which takes both
	 * lengths, so both copies reach the guard. */
	{
		CHK(reac_frame_clean_len(REAC_FRAME_BYTES + 2) == (size_t)REAC_FRAME_BYTES);

		char path[] = "/tmp/reacpw-dup-dstwin-XXXXXX";
		int fd = mkstemp(path);
		CHK(fd >= 0);
		FILE *f = fdopen(fd, "wb");
		CHK(f != NULL);
		pcap_hdr(f);
		uint8_t down[REAC_FRAME_BYTES + 2];
		for (int i = 0; i < NFRAMES; i++) {
			mk_downstream(down, (uint16_t)(CTR_BASE + i));
			pcap_rec(f, down, REAC_FRAME_BYTES);
			add_fcs_residue(down, REAC_FRAME_BYTES);
			pcap_rec(f, down, REAC_FRAME_BYTES + 2);
		}
		fclose(f);

		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, NFRAMES) == 0);
		uint64_t ok  = atomic_load(&rx.frames_ok);
		uint64_t dup = atomic_load(&rx.frames_dup);
		CHK(ok >= NFRAMES);
		CHK(dup == ok || dup + 1 == ok);
		CHK(dup >= NFRAMES - 1);
		CHK(atomic_load(&rx.frames_bad) == 0);
		CHK(atomic_load(&rx.counter_gaps) == 0);
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
		                           .forced_rate = REAC_SAMPLE_RATE_96K, .pcap_realtime = 0,
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
	printf("OK: rx dup guard — the over-clock repeat (same length) AND the mirror twin "
	       "(628/630 upstream, 1492/1494 downstream, differing only by the FCS residue) "
	       "each drop exactly one copy per frame across the counter seam, the ring "
	       "carries the single cadence in order, and distinct frames at doubled cadence "
	       "are untouched (dup == 0)\n");
	return 0;
}
