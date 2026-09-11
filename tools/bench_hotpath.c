// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* bench_hotpath — per-frame CPU cost of the two hot paths, measured on the REAL
 * functions.
 *
 * The rule this file exists to honour: a benchmark that re-implements the thing
 * it measures proves nothing about the thing it measures. So the RX side is
 * measured by `#include "reac_rx.c"` — feed_frame is static, and including the
 * translation unit is the only way to call the shipping code rather than a copy
 * of it. Any edit to feed_frame changes this number by construction; a copy
 * would have to be kept in step by hand, and would not be.
 *
 * Timing is CLOCK_THREAD_CPUTIME_ID, not wall time: this host also carries a
 * live REAC rig, so wall time measures the scheduler as much as the code. Each
 * case runs a warm-up pass that is discarded, then REPS passes whose MEDIAN is
 * reported — a mean is a hostage to one preemption.
 *
 * Cases:
 *   rx_feed   one downstream frame, wire to ring: decode + planar zero + s24->f32
 *             + ring write. Copies 3/4/5 of the fast-path spec's table.
 *   rx_dup    the duplicate guard's memcmp + memcpy over the clean length (copy 2).
 *   tx_pop    the pacer's frame-ring pop into popbuf plus the popbuf->frame
 *             memcpy the emit path does (the redundant TX copy).
 *
 * Output is one `key=value` line per case so a diff of two runs is mechanical.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <reac/reac.h>
#include <reac/reac_braid.h>

/* The unit under test, included whole so the static hot function is reachable.
 * reac_rx.c moved to libreac-transport (docs/design/specs/2026-09-11-reac-transport-library.md);
 * only its HEADER ships publicly, so reaching the .c for this trick needs a sibling libreac
 * checkout's transport/src on the include path — meson.build's LIBREAC_TRANSPORT_SRCDIR
 * option, dev-only, which is why this target is not build_by_default any more. */
#include "reac_rx.c"
#include <reac/transport/reac_pacer.h>
#include "upstream_fixtures.inc"

#define FRAMES  20000   /* per pass */
#define REPS       11   /* odd, so the median is a measured pass */

static uint64_t cpu_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

/* A well-formed 40-channel downstream broadcast, distinct per counter so the
 * decode has real work and the dup guard never short-circuits. Same fixture
 * shape as tests/test_reac_rx_dup.c's mk_downstream. */
static void mk_downstream(uint8_t *out, uint16_t counter)
{
	memset(out, 0, REAC_FRAME_BYTES);
	memset(out, 0xff, 6);
	static const uint8_t master[6] = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
	memcpy(out + 6, master, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[14] = (uint8_t)(counter & 0xff);
	out[15] = (uint8_t)(counter >> 8);
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		for (int ch = 0; ch < 40; ch++) {
			size_t pos[3];
			reac_braid_pos(s, ch, 40, pos);
			uint8_t *audio = out + REAC_L2_HEADER_LEN;
			audio[pos[0]] = (uint8_t)(counter + ch);
			audio[pos[1]] = (uint8_t)(s * 7 + ch);
			audio[pos[2]] = (uint8_t)(0x10 + ch);
		}
	}
	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

/* ---- case: rx_feed ------------------------------------------------------ *
 * feed_frame over a ring the consumer keeps drained. Draining matters: a full
 * ring makes reac_ring_write return early and skip the very channel loop this
 * case exists to measure — the benchmark would then report the fix as free. */
static double case_rx_feed(int ring_channels, const char *label)
{
	static uint8_t frames[64][REAC_FRAME_BYTES];
	for (int i = 0; i < 64; i++)
		mk_downstream(frames[i], (uint16_t)(1000 + i));

	struct reac_ring ring;
	if (reac_ring_init(&ring, (uint32_t)ring_channels, 4096) != 0)
		return -1;
	struct reac_rx rx;
	memset(&rx, 0, sizeof rx);
	rx.ring = &ring;
	rx.sample_rate = 48000;
	rx.cfg.accept = REAC_RX_ACCEPT_DOWNSTREAM;
	const struct reac_mode *mode = reac_mode_for(48000);

	uint64_t pass[REPS];
	for (int rep = 0; rep < REPS + 1; rep++) {
		uint64_t t0 = cpu_ns();
		for (int i = 0; i < FRAMES; i++) {
			feed_frame(&rx, mode, frames[i & 63], REAC_FRAME_BYTES);
			if ((i & 63) == 63)
				reac_ring_trim(&ring, 64);   /* O(1): only moves tail */
		}
		uint64_t dt = cpu_ns() - t0;
		if (rep)                              /* rep 0 is the discarded warm-up */
			pass[rep - 1] = dt;
	}
	qsort(pass, REPS, sizeof pass[0], cmp_u64);
	reac_ring_free(&ring);
	double ns = (double)pass[REPS / 2] / FRAMES;
	printf("%s=%.1f\n", label, ns);
	return ns;
}

/* ---- case: rx_feed_upstream --------------------------------------------- *
 * What the rig actually runs. In master role the feeder's accept is UPSTREAM —
 * the box's 340-byte 8-channel return, not the 1492-byte 40-channel broadcast —
 * so the decode and the float conversion are 8 channels wide while the ring
 * write is whatever the ring was allocated with. That gap IS the "40 channels
 * for an 8-channel box" finding, and this case is where it shows. UP8 is the
 * captured S-0808 return, the same fixture the dup/gate tests use. */
static double case_rx_feed_upstream(int ring_channels, const char *label)
{
	static uint8_t frames[64][sizeof UP8];
	for (int i = 0; i < 64; i++) {
		memcpy(frames[i], UP8, sizeof UP8);
		frames[i][14] = (uint8_t)i;          /* distinct counter per frame */
	}
	struct reac_ring ring;
	if (reac_ring_init(&ring, (uint32_t)ring_channels, 4096) != 0)
		return -1;
	struct reac_rx rx;
	memset(&rx, 0, sizeof rx);
	rx.ring = &ring;
	rx.sample_rate = 48000;
	rx.cfg.accept = REAC_RX_ACCEPT_UPSTREAM;
	const struct reac_mode *mode = reac_mode_for(48000);

	uint64_t pass[REPS];
	for (int rep = 0; rep < REPS + 1; rep++) {
		uint64_t t0 = cpu_ns();
		for (int i = 0; i < FRAMES; i++) {
			feed_frame(&rx, mode, frames[i & 63], sizeof UP8);
			if ((i & 63) == 63)
				reac_ring_trim(&ring, 64);
		}
		uint64_t dt = cpu_ns() - t0;
		if (rep)
			pass[rep - 1] = dt;
	}
	qsort(pass, REPS, sizeof pass[0], cmp_u64);
	/* A benchmark that measured a rejected frame would report any change as
	 * free. Prove the path ran: every frame must have been accepted. */
	if (atomic_load(&rx.frames_ok) == 0 || atomic_load(&rx.frames_bad) != 0) {
		fprintf(stderr, "bench: upstream case decoded nothing (ok=%llu bad=%llu)\n",
		        (unsigned long long)atomic_load(&rx.frames_ok),
		        (unsigned long long)atomic_load(&rx.frames_bad));
		exit(1);
	}
	reac_ring_free(&ring);
	double ns = (double)pass[REPS / 2] / FRAMES;
	printf("%s=%.1f\n", label, ns);
	return ns;
}

/* ---- case: rx_dup ------------------------------------------------------- *
 * The guard as the feeder runs it: compare the clean prefix against the stored
 * previous frame, then store this one. Distinct frames, so the memcmp always
 * runs to a mismatch late in the buffer and the memcpy always happens — the
 * steady state on a live wire. */
static double case_rx_dup(void)
{
	static uint8_t frames[64][REAC_FRAME_BYTES];
	for (int i = 0; i < 64; i++)
		mk_downstream(frames[i], (uint16_t)(1000 + i));
	struct reac_rx rx;
	memset(&rx, 0, sizeof rx);

	uint64_t pass[REPS];
	uint64_t sink = 0;
	for (int rep = 0; rep < REPS + 1; rep++) {
		uint64_t t0 = cpu_ns();
		for (int i = 0; i < FRAMES; i++) {
			const uint8_t *f = frames[i & 63];
			size_t clean = reac_frame_clean_len(REAC_FRAME_BYTES);
			if (rx.have_prev_frame && clean == rx.prev_clean_len &&
			    memcmp(f, rx.prev_frame, clean) == 0)
				sink++;
			if (clean <= sizeof rx.prev_frame) {
				memcpy(rx.prev_frame, f, clean);
				rx.prev_clean_len = clean;
				rx.have_prev_frame = 1;
			}
		}
		uint64_t dt = cpu_ns() - t0;
		if (rep)
			pass[rep - 1] = dt;
	}
	qsort(pass, REPS, sizeof pass[0], cmp_u64);
	double ns = (double)pass[REPS / 2] / FRAMES;
	printf("rx_dup_ns=%.1f (dups=%llu)\n", ns, (unsigned long long)sink);
	return ns;
}

/* ---- case: tx_pop ------------------------------------------------------- *
 * The pacer's emit-side staging: pop the oldest frame out of the SPSC frame ring
 * into popbuf, then memcpy popbuf -> frame. Reproduced here rather than included
 * because reac_pacer.c drags the whole master FSM in; the two calls below ARE the
 * two lines of reac_pacer.c's loop, and the ring functions are the shipping ones. */
static double case_tx_pop(int with_memcpy)
{
	struct reac_frame_ring fr;
	if (reac_frame_ring_init(&fr, 1024, REAC_FRAME_BYTES) != 0)
		return -1;
	uint8_t src[REAC_FRAME_BYTES];
	mk_downstream(src, 7);
	uint8_t popbuf[2048], frame[2048];
	uint64_t pass[REPS];

	for (int rep = 0; rep < REPS + 1; rep++) {
		uint64_t t0 = cpu_ns();
		for (int i = 0; i < FRAMES; i++) {
			reac_frame_ring_push(&fr, src, REAC_FRAME_BYTES);
			if (with_memcpy) {
				uint16_t n = reac_frame_ring_pop(&fr, popbuf);
				if (n >= REAC_FRAME_BYTES)
					memcpy(frame, popbuf, REAC_FRAME_BYTES);
			} else {
				reac_frame_ring_pop(&fr, frame);
			}
		}
		uint64_t dt = cpu_ns() - t0;
		if (rep)
			pass[rep - 1] = dt;
	}
	qsort(pass, REPS, sizeof pass[0], cmp_u64);
	reac_frame_ring_free(&fr);
	double ns = (double)pass[REPS / 2] / FRAMES;
	printf("tx_pop%s_ns=%.1f\n", with_memcpy ? "_via_popbuf" : "_direct", ns);
	return ns;
}

int main(int argc, char **argv)
{
	int width = argc > 1 ? atoi(argv[1]) : 40;
	printf("# bench_hotpath frames=%d reps=%d clock=THREAD_CPUTIME\n", FRAMES, REPS);
	case_rx_feed(REAC_MAX_CHANNELS, "rx_down_ring40_ns");
	if (width != REAC_MAX_CHANNELS)
		case_rx_feed(width, "rx_down_ringN_ns");
	case_rx_feed_upstream(REAC_MAX_CHANNELS, "rx_up8_ring40_ns");
	if (width != REAC_MAX_CHANNELS)
		case_rx_feed_upstream(width, "rx_up8_ringN_ns");
	case_rx_dup();
	case_tx_pop(1);
	case_tx_pop(0);
	return 0;
}
