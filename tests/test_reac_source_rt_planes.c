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
	int whole = fx_skip_whole(c, 0);
	if (bad != (uint32_t)fx_counted(c, 0)) {
		fprintf(stderr, "FAIL: %s: %u cycles counted bad, want %d\n", argv[1], bad,
		        fx_counted(c, 0));
		return 1;
	}
	if (whole ? after != before : after >= before) {
		fprintf(stderr, "FAIL: %s: ring %u -> %u (a %s cycle)\n", argv[1], before, after,
		        whole ? "skipped" : "played");
		return 1;
	}
	/* Every plane the node can carry PLAYS (0.5 from the ring); a bad plane is left
	 * as it was and its chunk, when there is one, says nothing in it. */
	if (!whole)
		for (int p = 0; p < 4 && (uint32_t)p < fx_sbuf.n_datas; p++) {
			struct spa_data *d = &fx_sbuf.datas[p];
			if (fx_plane_good(c, p, 0)) {
				if (d->chunk->size != FX_FLOATS * 4)
					goto bad_plane;
				for (uint32_t k = 0; k < FX_FLOATS; k++)
					if (((float *)d->data)[k] != 0.5f)
						goto bad_plane;
			} else {
				if (d->chunk && d->chunk->size != 0)
					goto bad_plane;
				for (uint32_t k = 0; d->data && k < FX_SHORT_FLOATS; k++)
					if (((float *)d->data)[k] != FX_LEVEL(p))
						goto bad_plane;
			}
			continue;
bad_plane:
			fprintf(stderr, "FAIL: %s: plane %d did not come out as a %s plane must\n",
			        argv[1], p, fx_plane_good(c, p, 0) ? "good" : "bad");
			return 1;
		}
	printf("ok %s: ring %u -> %u, bad buffers %u\n", argv[1], before, after, bad);
	return 0;
}
