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
 * So the callback asks here, plane by plane, before it touches a byte. A bad PLANE is
 * silent and the others play: the sink reads it as zeros, the source leaves it
 * unwritten (its chunk saying zero bytes). Only a buffer with no planes array at all,
 * or the sink's missing plane-0 chunk (which sizes the cycle), costs the whole cycle.
 * Either way the cycle is counted once, in the node's atomic counter, which the main
 * loop reads and publishes as REACPW_PROP_HEALTH_RT_BAD_BUFFERS. Nothing aborts, logs or
 * allocates: header-only, no syscall, no state of its own. */
#include <stdbool.h>
#include <stdint.h>
#include <spa/buffer/buffer.h>

/* The node property the counter is published under (a count of cycles, since start). */
#define REACPW_PROP_HEALTH_RT_BAD_BUFFERS "reac.health.rt-bad-buffers"

/* There is a planes array to look into at all. */
static inline bool reac_rt_buffer_usable(const struct spa_buffer *b)
{
	return b != NULL && b->datas != NULL && b->n_datas > 0;
}

/* Plane `c` exists and is usable for `bytes` of audio: it has a data pointer, a chunk
 * pointer and room for `bytes` in its OWN maxsize; for an INPUT plane (`valid`) its own
 * chunk must also say that many bytes are valid. */
static inline bool reac_rt_plane_ok(const struct spa_buffer *b, int c, uint32_t bytes, bool valid)
{
	if (c < 0 || (uint32_t)c >= b->n_datas)
		return false;
	const struct spa_data *d = &b->datas[c];
	if (d->data == NULL || d->chunk == NULL || d->maxsize < bytes)
		return false;
	return !valid || d->chunk->size >= bytes;
}

#endif
