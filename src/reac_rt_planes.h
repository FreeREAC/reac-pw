/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
#ifndef REAC_RT_PLANES_H
#define REAC_RT_PLANES_H

/* WHAT THE REALTIME CALLBACKS MAY TRUST ABOUT A STREAM BUFFER: NOTHING.
 *
 * The sink's and the source's on_process size a cycle from plane 0 and then touch
 * every plane. A buffer with fewer planes than the node has channels, a plane whose
 * data or chunk pointer is absent, or a plane shorter than the cycle needs would be
 * read or written out of bounds on the RT thread. The adapter does not hand such a
 * buffer today; a graph that is being torn down, a format renegotiation and a
 * misbehaving peer are the ways it could.
 *
 * So the callback asks here, before it touches a byte. A bad buffer costs the cycle
 * (the callback queues it back and returns; the pacer free-runs FILLER, the ring keeps
 * its frames) and ONE relaxed increment of the node's counter, which the main loop
 * reads (reac_sink_node_bad_buffers / reac_source_node_bad_buffers). It never aborts,
 * never logs and never allocates: header-only, no syscall, no state of its own. */
#include <stdbool.h>
#include <stdint.h>
#include <spa/buffer/buffer.h>

/* One plane is usable for `bytes` of audio: it has a data pointer, a chunk pointer and
 * room for `bytes` in its OWN maxsize; for an INPUT plane (`valid`) its own chunk must
 * also say that many bytes are valid. */
static inline bool reac_rt_plane_ok(const struct spa_data *d, uint32_t bytes, bool valid)
{
	if (d->data == NULL || d->chunk == NULL || d->maxsize < bytes)
		return false;
	return !valid || d->chunk->size >= bytes;
}

/* The buffer carries at least `channels` planes and plane 0 can be looked at, so a
 * caller may read datas[0] to size the cycle. */
static inline bool reac_rt_buffer_has_planes(const struct spa_buffer *b, int channels)
{
	return b != NULL && b->datas != NULL && channels > 0 && b->n_datas >= (uint32_t)channels;
}

/* Every plane the node touches (the first `channels`) is usable for `bytes` (`valid`:
 * as input), and every plane the node stamps (all n_datas, when `stamp_all`) has a chunk
 * to stamp. */
static inline bool reac_rt_planes_ok(const struct spa_buffer *b, int channels, uint32_t bytes,
                                     bool valid, bool stamp_all)
{
	if (!reac_rt_buffer_has_planes(b, channels))
		return false;
	for (int c = 0; c < channels; c++)
		if (!reac_rt_plane_ok(&b->datas[c], bytes, valid))
			return false;
	if (stamp_all)
		for (uint32_t c = (uint32_t)channels; c < b->n_datas; c++)
			if (b->datas[c].chunk == NULL)
				return false;
	return true;
}

#endif
