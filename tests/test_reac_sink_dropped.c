// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The sink's realtime callback when the ring behind it is full. on_process stages
 * REAC_SAMPLES_PER_PKT frames per packet and hands each packet to the downstream
 * ring (reac_ring_write on a slave's upstream ring, reac_pacer_submit on the master's
 * pacer). Both can refuse; the return values were dropped, so audio vanished with only
 * the ring's own counter knowing. The callback now counts what was refused.
 *
 * The postcondition: frames staged == frames the ring took + frames counted dropped.
 * argv[1] names the path: upstream-ring or pacer-ring. */
#include "reac_sink_node.c"

#include <stdio.h>
#pragma GCC diagnostic ignored "-Wunused-function"
#include "rt_buffer_fixture.inc"

#define CYCLES 3   /* three good buffers of FX_FLOATS frames each */

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	int pacer = strcmp(argv[1], "pacer-ring") == 0;
	if (!pacer && strcmp(argv[1], "upstream-ring") != 0)
		return 2;

	struct reac_sink_node *n = calloc(1, sizeof *n);
	struct reac_ring ring;
	if (!n)
		return 2;
	n->channels = 4;
	n->ramp_step = 1.0f;
	for (int i = 0; i < 4; i++) {
		atomic_init(&n->chan_target[i], 1.0f);
		n->chan_cur[i] = 1.0f;
	}
	if (pacer) {
		/* 8 slots hold 7 packets: the 8th push onward is refused */
		if (reac_frame_ring_init(&n->pacer.ring, 8, REAC_FRAME_BYTES) != 0)
			return 2;
		n->pacer_open = 1;
	} else {
		/* room for 8 packets' frames: the 9th is short */
		if (reac_ring_init(&ring, 4, 8 * REAC_SAMPLES_PER_PKT) != 0)
			return 2;
		n->upstream_ring = &ring;
	}
	struct spa_io_position p;
	memset(&p, 0, sizeof p);
	p.clock.duration = FX_FLOATS;
	n->position = &p;
	n->stream = (struct pw_stream *)&p;   /* only ever handed to the stubs */
	n->dropped_last = -1;

	for (int i = 0; i < CYCLES; i++) {
		fx_build_case(FX_GOOD_BUF, 0);
		on_process(n);
	}

	uint32_t staged = (CYCLES * FX_FLOATS / REAC_SAMPLES_PER_PKT) * REAC_SAMPLES_PER_PKT;
	uint32_t took = pacer ? reac_frame_ring_readable(&n->pacer.ring) * REAC_SAMPLES_PER_PKT
	                      : reac_ring_readable(&ring);
	uint32_t dropped = reac_sink_node_dropped_frames(n);
	printf("%s: staged %u, ring took %u, counted dropped %u\n", argv[1], staged, took,
	       dropped);
	if (took >= staged) {
		fprintf(stderr, "FAIL: the ring was meant to fill\n");
		return 1;
	}
	if (took + dropped != staged) {
		fprintf(stderr, "FAIL: staged %u != took %u + dropped %u\n", staged, took, dropped);
		return 1;
	}
	/* THE COUNTER IS PUBLISHED off the RT thread, and the published value MOVES. */
	char want[32];
	sink_publish_dropped_frames(n);
	snprintf(want, sizeof want, "%u", dropped);
	if (fx_drop_calls != 1 || strcmp(fx_drop_val, want) != 0) {
		fprintf(stderr, "FAIL: published '%s' after %d calls, want %s\n", fx_drop_val,
		        fx_drop_calls, want);
		return 1;
	}
	sink_publish_dropped_frames(n);   /* unmoved: nothing stamped again */
	if (fx_drop_calls != 1) {
		fprintf(stderr, "FAIL: an unmoved counter was stamped again\n");
		return 1;
	}
	fx_build_case(FX_GOOD_BUF, 0);
	on_process(n);
	uint32_t dropped2 = reac_sink_node_dropped_frames(n);
	sink_publish_dropped_frames(n);
	snprintf(want, sizeof want, "%u", dropped2);
	if (dropped2 <= dropped || fx_drop_calls != 2 || strcmp(fx_drop_val, want) != 0) {
		fprintf(stderr, "FAIL: dropped %u after another cycle (was %u), published '%s' after "
		        "%d calls, want %s\n", dropped2, dropped, fx_drop_val, fx_drop_calls, want);
		return 1;
	}
	printf("published %s\n", fx_drop_val);
	return 0;
}
