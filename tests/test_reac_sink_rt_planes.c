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

	fx_build_case(c, 0);
	on_process(n);

	/* Every dequeued buffer is owed back, good or bad. */
	if (fx_queued != 1) {
		fprintf(stderr, "FAIL: %s: buffer queued %d times, want 1\n", argv[1], fx_queued);
		return 1;
	}
	uint32_t got = reac_ring_readable(&ring);
	if (c == FX_GOOD_BUF && got == 0) {
		fprintf(stderr, "FAIL: good buffer produced nothing\n");
		return 1;
	}
	printf("ok %s: ring frames %u\n", argv[1], got);
	return 0;
}
