// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* tap-probe — run the REAL passive engine (reac_tap_open + reac_tap_start, live
 * AF_PACKET on a real interface) for a few seconds and report what it SERVED.
 *
 * It exists to be measured from the OTHER end of a veth pair. The claim under test is
 * an ABSENCE — "a segment in tap role sends zero frames" (openmixer master-arbitration,
 * eighth amendment, 2026-09-13) — and an absence is only a finding once the instrument
 * has been shown to detect the corresponding presence, which is what
 * tests/tap-sends-nothing.sh's control arm is for: the SAME ear, the SAME filter, with
 * the real slave engine on this end instead of this one.
 *
 * IT ALSO REPORTS WHAT IT HEARD, and that is not decoration. A program that crashed at
 * open sends zero frames too. The harness requires streams >= 1, a measured rate and a
 * moving frame count before it reads the silence as a tap's silence.
 *
 *   tap-probe <iface> <secs>
 */
#include <reac/transport/reac_tap.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop_now;
static void on_sig(int s) { (void)s; stop_now = 1; }

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <iface> <secs>\n", argv[0]);
		return 2;
	}
	const char *iface = argv[1];
	const int secs = atoi(argv[2]);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	struct reac_tap tap;
	struct reac_tap_cfg cfg = {
		.kind = REAC_RX_LIVE,
		.source = iface,
		.forced_rate = 0,     /* the master's cadence decides, as a slave's does */
		.survey_ms = 1500,
	};
	if (reac_tap_open(&tap, &cfg) != 0) {
		fprintf(stderr, "tap-probe: reac_tap_open failed on %s\n", iface);
		return 1;
	}
	printf("streams %u\n", reac_tap_stream_count(&tap));
	printf("rate %d\n", tap.sample_rate);
	for (unsigned i = 0; i < tap.n; i++) {
		const struct reac_tap_stream *s = reac_tap_stream_at(&tap, i);
		printf("stream %u %s %02x:%02x:%02x:%02x:%02x:%02x %u\n", i,
		       s->kind == REAC_TAP_STREAM_MASTER ? "master" : "box",
		       s->src[0], s->src[1], s->src[2], s->src[3], s->src[4], s->src[5],
		       s->channels);
	}
	if (reac_tap_start(&tap) != 0) {
		fprintf(stderr, "tap-probe: reac_tap_start failed\n");
		reac_tap_close(&tap);
		return 1;
	}
	for (int i = 0; i < secs * 10 && !stop_now; i++) {
		struct timespec ts = { 0, 100000000 };   /* 100 ms */
		nanosleep(&ts, NULL);
	}
	unsigned long long ok = 0;
	for (unsigned i = 0; i < tap.n; i++)
		ok += atomic_load_explicit(&tap.rx[i].frames_ok, memory_order_relaxed);
	printf("served_frames %llu\n", ok);
	reac_tap_stop(&tap);
	reac_tap_close(&tap);
	return 0;
}
