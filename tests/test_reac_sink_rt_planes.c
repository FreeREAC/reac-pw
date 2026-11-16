// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The sink's REALTIME callback against a buffer that is not what it expects.
 * on_process read datas[0].chunk->size to size every plane, and read plane c's
 * samples up to that count, trusting n_datas, each data pointer, each chunk and
 * each plane's length. A buffer with no planes, an absent chunk or a plane shorter
 * than plane 0 was read out of bounds on the RT thread.
 *
 * The callback is static, so this file includes the node's source and drives it
 * with a fake pw_stream pool (rt_buffer_fixture.inc) whose allocations are exactly
 * as large as the buffer claims; built with AddressSanitizer, the first stray
 * byte is a report. argv[1] names the case. */
#include "reac_sink_node.c"

#include <stdio.h>
#include "rt_buffer_fixture.inc"

int main(int argc, char **argv)
{
	if (argc != 2 || fx_case_by_name(argv[1]) < 0) {
		fprintf(stderr, "usage: %s <case>\n", argv[0]);
		return 2;
	}
	enum fx_case c = (enum fx_case)fx_case_by_name(argv[1]);

	struct reac_sink_node *n = calloc(1, sizeof *n);
	struct reac_ring ring;
	if (!n || reac_ring_init(&ring, 4, 4096) != 0)
		return 2;
	n->channels = 4;
	n->upstream_ring = &ring;      /* the slave's door: no pacer needed */
	n->ramp_step = 1.0f;
	for (int i = 0; i < 4; i++) {
		atomic_init(&n->chan_target[i], 1.0f);
		n->chan_cur[i] = 1.0f;
	}
	struct spa_io_position pos;
	memset(&pos, 0, sizeof pos);
	pos.clock.duration = FX_FLOATS;
	n->position = &pos;
	n->stream = (struct pw_stream *)&pos;   /* only ever handed to the stubs */
	n->bad_buffers_last = -1;

	fx_build_case(c, 0);
	on_process(n);

	/* Every dequeued buffer is owed back, good or bad. */
	if (fx_queued != 1) {
		fprintf(stderr, "FAIL: %s: buffer queued %d times, want 1\n", argv[1], fx_queued);
		return 1;
	}
	uint32_t got = reac_ring_readable(&ring);
	uint32_t bad = reac_sink_node_bad_buffers(n);
	int whole = fx_skip_whole(c, 1);
	if (bad != (uint32_t)fx_counted(c, 1)) {
		fprintf(stderr, "FAIL: %s: %u cycles counted bad, want %d\n", argv[1], bad,
		        fx_counted(c, 1));
		return 1;
	}
	if (whole ? got != 0 : got == 0) {
		fprintf(stderr, "FAIL: %s: ring holds %u frames (a %s cycle)\n", argv[1], got,
		        whole ? "skipped" : "played");
		return 1;
	}
	/* A bad plane is SILENT and every other plane PLAYS: read what reached the ring. */
	if (!whole) {
		static float out[4][4096];
		float *dst[4] = { out[0], out[1], out[2], out[3] };
		reac_ring_read_planar(&ring, dst, 4, got);
		for (int p = 0; p < 4; p++) {
			float want = fx_plane_good(c, p, 1) ? FX_LEVEL(p) : 0.0f;
			for (uint32_t k = 0; k < got; k++)
				if (out[p][k] != want) {
					fprintf(stderr, "FAIL: %s: plane %d frame %u is %g, want %g\n",
					        argv[1], p, k, out[p][k], want);
					return 1;
				}
		}
	}
	/* THE COUNTER IS PUBLISHED, off the RT thread, and the published value MOVES: a
	 * second cycle of the same buffer moves it by the same step. */
	sink_publish_bad_buffers(n);
	if (!fx_published_is(bad)) {
		fprintf(stderr, "FAIL: %s: published '%s' after %d calls, want %u\n", argv[1],
		        fx_prop_val, fx_prop_calls, bad);
		return 1;
	}
	fx_build_case(c, 0);
	on_process(n);
	sink_publish_bad_buffers(n);
	if (!fx_published_is(2 * bad)) {
		fprintf(stderr, "FAIL: %s: published '%s' after a second cycle, want %u\n", argv[1],
		        fx_prop_val, 2 * bad);
		return 1;
	}
	printf("ok %s: ring frames %u, bad buffers %u, published %s\n", argv[1], got, bad,
	       fx_prop_val);
	return 0;
}
