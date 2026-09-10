// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE BOX MASTER'S OWN FRAMES, off the rig, through the path that must carry them.
 *
 * Operator ruling, 2026-09-10: "the clock owner has nothing to do with normal audio
 * operations, only enrolment" and "reac-pw should simply connect it" — on a segment a
 * stagebox masters, its broadcast slots land on reac-capture's ports exactly as a slave
 * box's upstream does when we master it. No role-specific audio path.
 *
 * WHY THIS EXISTS BESIDE test_reac_box_master_audio.c. That one feeds the path frames
 * built by reac_ctrl_build_flood_filler — OUR builder — so it proves the decoder agrees
 * with our encoder and nothing more. On 2026-09-10 the console's meters for a real
 * S-1608 on M read a flat floor while that test was green, and the question "is the
 * daemon dropping this box's audio?" could not be answered from inside the repo. It can
 * now: the bytes in tests/box_master_frames.inc are the box's own, verbatim.
 *
 * WHAT IT MEASURED, and it is a fact worth keeping rather than a threshold to tune. Fed
 * through the real feeder at the accept mode main.c sets for a joined box master, all six
 * frames are accepted, none is rejected as the other stream, and all sixteen of the box's
 * inputs arrive on ring rows 0..15 carrying INDEPENDENT converter noise at about
 * -102.5 dBFS — sixteen quiet preamps, decoded correctly. Nothing louder was on that
 * wire. So the daemon lands what the box sends; the reason a microphone was inaudible
 * that morning is upstream of the wire, at preamps that never received their settings,
 * and it is not a defect in this path. The assertions below are written to that: they
 * pin the level BAND, so a decode that broke into sign-extension garbage (the historical
 * plain-LE smear, orders of magnitude louder) fails, and so does one that dropped to
 * digital silence.
 *
 * THE PROBE CAN DETECT PRESENCE, proven in the same run and on real bytes: SLAVE_LOUD_16
 * is the same chassis as an ordinary slave with music on its input 16, and through this
 * same path input 16 arrives tens of dB above that floor. Without that control, "the
 * floor is what the box sent" and "the decoder outputs a floor whatever it is given" read
 * identically.
 *
 * No sockets, no PipeWire: the real feeder thread over a temp pcap, as test_reac_rx_gate.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <reac/reac.h>

#include "reac_ring.h"
#include "reac_rx.h"

#include "box_master_frames.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define BOX_CH 16

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

/* Write `reps` copies of `n` real frames to a temp pcap. The feeder's duplicate guard
 * compares ADJACENT frames, and these six differ from each other (distinct counters), so
 * repeating the run gives the ring enough samples without a single dup being dropped. */
static void write_pcap(const char *path, const uint8_t (*fr)[628], int n, int reps)
{
	FILE *f = fopen(path, "wb");
	CHK(f != NULL);
	if (!f)
		return;
	pcap_hdr(f);
	for (int r = 0; r < reps; r++)
		for (int i = 0; i < n; i++)
			pcap_rec(f, fr[i], 628);
	fclose(f);
}

/* Drain the feeder into a per-channel sum of squares, so the level is measured over
 * everything that arrived rather than over one lucky block. */
static int run_and_measure(const char *path, double rms_out[REAC_MAX_CHANNELS],
                           struct reac_rx *rx_out, long *samples_out)
{
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = 48000, .pcap_realtime = 0,
	                           /* the accept mode main.c sets for a joined box master */
	                           .accept = REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	if (reac_rx_open(&rx, &cfg, &ring) != 0)
		return -1;
	if (reac_rx_start(&rx) != 0) {
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		return -1;
	}
	double sum[REAC_MAX_CHANNELS] = { 0 };
	long total = 0;
	float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *dst[REAC_MAX_CHANNELS];
	for (int c = 0; c < REAC_MAX_CHANNELS; c++)
		dst[c] = ch[c];
	for (int i = 0; i < 4000 && total < 1200; i++) {
		int n = reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS,
		                              REAC_SAMPLES_PER_PKT);
		if (n > 0) {
			for (int c = 0; c < REAC_MAX_CHANNELS; c++)
				for (int s = 0; s < n; s++)
					sum[c] += (double)ch[c][s] * ch[c][s];
			total += n;
		} else {
			struct timespec ts = { 0, 500000 };
			nanosleep(&ts, NULL);
		}
	}
	reac_rx_stop(&rx);
	for (int c = 0; c < REAC_MAX_CHANNELS; c++)
		rms_out[c] = total ? sqrt(sum[c] / (double)total) : 0.0;
	*rx_out = rx;                 /* the counters, read after the thread has joined */
	*samples_out = total;
	reac_rx_close(&rx);
	reac_ring_free(&ring);
	return 0;
}

static double dbfs(double rms) { return rms > 0 ? 20.0 * log10(rms) : -999.0; }

int main(void)
{
	char pm[] = "/tmp/reacpw-bmreal-XXXXXX";
	char pl[] = "/tmp/reacpw-bmloud-XXXXXX";
	int fdm = mkstemp(pm), fdl = mkstemp(pl);
	CHK(fdm >= 0 && fdl >= 0);
	close(fdm); close(fdl);

	const int NBM = (int)(sizeof BOX_MASTER_16 / sizeof BOX_MASTER_16[0]);
	const int NLD = (int)(sizeof SLAVE_LOUD_16 / sizeof SLAVE_LOUD_16[0]);
	CHK(NBM >= 4 && NLD >= 4);
	write_pcap(pm, BOX_MASTER_16, NBM, 40);
	write_pcap(pl, SLAVE_LOUD_16, NLD, 40);

	/* ---- 1. THE POSITIVE CONTROL FIRST: can this path carry a signal at all? ---- */
	double loud[REAC_MAX_CHANNELS];
	struct reac_rx rxl;
	long nl = 0;
	CHK(run_and_measure(pl, loud, &rxl, &nl) == 0);
	CHK(atomic_load(&rxl.frames_ok) >= (uint64_t)NLD);
	CHK(atomic_load(&rxl.frames_bad) == 0);
	CHK(nl > 0);
	/* Input 16 carried the music; the other fifteen were idle preamps. A path that
	 * flattened everything to a floor would fail this, and so would one that smeared
	 * the loud channel across all sixteen. */
	double quiet_max = 0;
	for (int c = 0; c < BOX_CH - 1; c++)
		if (loud[c] > quiet_max)
			quiet_max = loud[c];
	CHK(dbfs(loud[BOX_CH - 1]) > -60.0);
	CHK(dbfs(loud[BOX_CH - 1]) - dbfs(quiet_max) > 30.0);

	/* ---- 2. THE BOX ON M: every one of its sixteen inputs lands on the ring ---- */
	double bm[REAC_MAX_CHANNELS];
	struct reac_rx rxm;
	long nm = 0;
	CHK(run_and_measure(pm, bm, &rxm, &nm) == 0);
	/* PRESENCE BEFORE VALUE: a gate that rejected every frame and a silent box read the
	 * same downstream, so the accept counters are asserted before the samples are. */
	CHK(atomic_load(&rxm.frames_ok) >= (uint64_t)NBM);
	CHK(atomic_load(&rxm.frames_bad) == 0);
	CHK(atomic_load(&rxm.frames_other) == 0);   /* the box's own stream, nothing else */
	CHK(nm > 0);

	/* THE JOB: sixteen inputs, none of them silent, none of them garbage, and none of
	 * them a copy of another. -130 dBFS is far below any real converter and -60 dBFS far
	 * above what sixteen idle preamps produce, so the band fails a decode that collapsed
	 * to zero AND one that broke into the plain-LE sign-extension smear. */
	for (int c = 0; c < BOX_CH; c++) {
		CHK(bm[c] > 0.0);
		CHK(dbfs(bm[c]) > -130.0);
		CHK(dbfs(bm[c]) < -60.0);
	}
	/* SIXTEEN INDEPENDENT CONVERTERS, not one row copied across. Two channels of the
	 * same chassis's noise are never bit-identical, so equal sums mean a duplicated row. */
	for (int c = 1; c < BOX_CH; c++)
		CHK(bm[c] != bm[c - 1]);
	/* AND NOTHING PAST THE BOX'S WIDTH. The node is sized to the width the box
	 * broadcast; the fabric's other 24 slots are not this box's and must read silent. */
	for (int c = BOX_CH; c < REAC_MAX_CHANNELS; c++)
		CHK(bm[c] == 0.0);

	/* THE MEASUREMENT, PRINTED. The band above is deliberately wide; the number is what
	 * a later reader compares against, and it is why "the box sent nothing audible" is a
	 * finding rather than a guess. */
	printf("box master on M, its own frames: %d accepted, 0 bad, 0 other; "
	       "16 inputs at %.1f .. %.1f dBFS. Control (same model as a slave, music on "
	       "input 16): %.1f dBFS against a %.1f dBFS floor.\n",
	       (int)atomic_load(&rxm.frames_ok), dbfs(bm[0]), dbfs(bm[BOX_CH - 1]),
	       dbfs(loud[BOX_CH - 1]), dbfs(quiet_max));

	unlink(pm);
	unlink(pl);
	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: a real box master's broadcast lands on all 16 capture rows, per channel, "
	       "by value — with a real-bytes positive control proving the path carries signal\n");
	return 0;
}
