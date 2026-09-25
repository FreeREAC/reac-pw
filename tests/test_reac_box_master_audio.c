// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE AUDIO A JOINED BOX MASTER DELIVERS, read back by value.
 *
 * DESIGN.md 0.5.1/0.5.2: a stagebox with its REAC Mode switch on M broadcasts its own
 * UPSTREAM geometry -- 52 + n*36 bytes, 340 B at 8 channels -- and the segment joins it
 * receive-only, decoding that stream through the SAME box-width gate and the SAME oracle
 * the master role uses for a box's return. The claim the operator cares about is not that
 * a node appeared at 8 channels; it is that the box's microphones arrive on it.
 *
 * WHY THIS IS THE SEAM THE VALUES ARE READ AT. The ring is what reac_source_node's
 * process() copies into the port planes, one row per port, and it is the last place the
 * samples exist before PipeWire owns them. The veth proof (tests/hearing-finds-a-segment.sh)
 * cannot read them further downstream: no session manager runs in its namespace, so no node
 * there ever materialises its ports and nothing schedules a process() to fill them. So the
 * audio is measured HERE, by value, per channel -- and the veth phase proves the daemon
 * accepts these very frames on a real wire.
 *
 * THE BYTES ARE THE FAKE BOX MASTER'S BYTES: reac_ctrl_build_flood_filler at the box's
 * width, the one builder tests/fake_box_master.c calls, so a change to the wire format
 * cannot leave this test asserting audio the daemon no longer speaks. The pattern is a
 * DISTINCT constant per channel (ch k carries (k+1)/64), which is what makes a cross-channel
 * mistake -- the braid read as plain LE, an off-by-one slot, a duplicated row -- fail here
 * rather than pass as "some audio arrived".
 *
 * No sockets, no PipeWire: the real feeder thread over a temp pcap, as test_reac_rx_gate.c.
 */
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
#include <reac/reac_ctrlblk.h>

#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define BOX_CH 8

/* ---- minimal classic-pcap writer (LE, linktype 1), as test_reac_rx_gate.c ---- */
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

/* The value channel k carries, everywhere in the fixture. Distinct per channel, well
 * inside 24-bit range, and none of them 0 -- a silent row is the failure this looks for. */
static float ch_value(int ch) { return (float)(ch + 1) / 64.0f; }

static int run_rx(struct reac_rx *rx, uint64_t want_ok)
{
	if (reac_rx_start(rx) != 0)
		return -1;
	for (int i = 0; i < 2000; i++) {          /* <= 2 s */
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
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t BOX[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };

	float pcm[BOX_CH][REAC_SAMPLES_PER_PKT];
	float *planar[BOX_CH];
	for (int c = 0; c < BOX_CH; c++) {
		planar[c] = pcm[c];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			pcm[c][s] = ch_value(c);
	}

	char path[] = "/tmp/reacpw-boxmaster-XXXXXX";
	int fd = mkstemp(path);
	CHK(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	CHK(f != NULL);
	pcap_hdr(f);

	uint8_t frame[2048];
	size_t flen = 0;
	for (uint16_t i = 0; i < 60; i++) {
		flen = reac_ctrl_build_flood_filler(frame, BCAST, BOX, i, BOX_CH,
		                                    planar, REAC_SAMPLES_PER_PKT);
		CHK(flen > 0);
		pcap_rec(f, frame, (uint32_t)flen);
	}
	fclose(f);

	/* The geometry the wire declares IS the classification and the frame size:
	 * 52 + 8*36 = 340 B (DESIGN.md 0.5.1, reac-protocol/wire-format.md). */
	CHK(flen == 340);
	CHK(flen == reac_ctrl_box_frame_len(BOX_CH));

	/* ---- the join: the receive-only accept mode main.c sets for a box master ---- */
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
	                           .forced_rate = REAC_SAMPLE_RATE_96K, .pcap_realtime = 0,
	                           .accept = REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
	CHK(run_rx(&rx, 40) == 0);

	/* PRESENCE BEFORE VALUE: a decoder that rejected every frame and a silent box read
	 * the same downstream, so the accept count is asserted before the samples are. */
	CHK(atomic_load(&rx.frames_ok) >= 40);
	CHK(atomic_load(&rx.frames_bad) == 0);
	CHK(atomic_load(&rx.frames_other) == 0);   /* a box master's own stream, nothing else */

	/* THE JOB: the box's eight microphones, on ring rows 0..7, each carrying ITS OWN
	 * value -- and every row past the box's width silent, because the node is sized to
	 * the width the box broadcast and the fabric's other 32 slots are not this box's. */
	float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *dst[REAC_MAX_CHANNELS];
	for (int c = 0; c < REAC_MAX_CHANNELS; c++)
		dst[c] = ch[c];
	CHK(reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT)
	    == REAC_SAMPLES_PER_PKT);
	for (int c = 0; c < BOX_CH; c++)
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			CHK(fabsf(ch[c][s] - ch_value(c)) < 1e-5f);
	for (int c = BOX_CH; c < REAC_MAX_CHANNELS; c++)
		CHK(ch[c][0] == 0.0f);

	/* THE PROBE CAN REPORT ABSENCE. The same read, one channel over, must NOT match:
	 * without this, a ring that returned channel 0 for every row would pass everything
	 * above. */
	CHK(fabsf(ch[1][0] - ch_value(0)) > 1e-3f);

	reac_rx_close(&rx);
	reac_ring_free(&ring);
	unlink(path);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: a box master's %d-ch broadcast decodes into the ring the capture node "
	       "reads, per channel, by value (%zu B frames, %d accepted, 0 bad)\n",
	       BOX_CH, flen, 40);
	return 0;
}
