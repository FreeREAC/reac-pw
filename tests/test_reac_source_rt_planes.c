// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The source's REALTIME callback against a buffer that is not what it expects.
 * on_process sized the cycle from datas[0].maxsize and then wrote that many
 * samples into every plane and stamped every chunk, trusting n_datas, each data
 * pointer, each chunk and each plane's own length. A buffer with no planes, an
 * absent chunk or a plane shorter than plane 0 was written out of bounds on the
 * RT thread.
 *
 * The callback is static, so this file includes the node's source and drives it
 * with a fake pw_stream pool (rt_buffer_fixture.inc) whose allocations are exactly
 * as large as the buffer claims; built with AddressSanitizer, the first stray
 * byte is a report. argv[1] names the case. */
#include "reac_source_node.c"

#include "rt_buffer_fixture.inc"

int main(int argc, char **argv)
{
	if (argc != 2 || fx_case_by_name(argv[1]) < 0) {
		fprintf(stderr, "usage: %s <case>\n", argv[0]);
		return 2;
	}
	enum fx_case c = (enum fx_case)fx_case_by_name(argv[1]);

	struct reac_source_node *n = calloc(1, sizeof *n);
	struct reac_rx *rx = calloc(1, sizeof *rx);
	struct reac_ring ring;
	if (!n || !rx || reac_ring_init(&ring, 4, 4096) != 0)
		return 2;
	float blk[4 * 64];
	for (size_t i = 0; i < sizeof blk / sizeof blk[0]; i++)
		blk[i] = 0.5f;
	for (int k = 0; k < 4; k++)
		reac_ring_write(&ring, blk, 64, 4);   /* 256 frames of 0.5 */
	n->ring = &ring;
	n->rx = rx;
	n->channels = 4;

	fx_short_slack = 1;
	fx_build_case(c, FX_FLOATS);
	uint32_t before = reac_ring_readable(&ring);
	on_process(n);

	if (fx_queued != 1) {
		fprintf(stderr, "FAIL: %s: buffer queued %d times, want 1\n", argv[1], fx_queued);
		return 1;
	}
	if (c == FX_SHORT_PLANE && !fx_short_plane_clean()) {
		fprintf(stderr, "FAIL: %s: wrote past the short plane's own size\n", argv[1]);
		return 1;
	}
	uint32_t after = reac_ring_readable(&ring);
	uint32_t bad = reac_source_node_bad_buffers(n);
	if (!fx_must_refuse(c, 0) && (after >= before || bad != 0)) {
		fprintf(stderr, "FAIL: good buffer: ring %u -> %u, %u counted bad\n", before, after, bad);
		return 1;
	}
	/* A bad buffer costs the cycle: the ring keeps its frames, one cycle is counted. */
	if (fx_must_refuse(c, 0) && (after != before || bad != 1)) {
		fprintf(stderr, "FAIL: %s: ring %u -> %u (want unchanged), %u counted bad (want 1)\n",
		        argv[1], before, after, bad);
		return 1;
	}
	printf("ok %s: ring %u -> %u, bad buffers %u\n", argv[1], before, after, bad);
	return 0;
}
