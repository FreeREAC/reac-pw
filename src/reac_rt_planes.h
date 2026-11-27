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
 * allocates: header-only, no syscall, no state of its own.
 *
 * The main-loop half of every such counter is here too (reac_rt_counter_publish): the
 * sink's two counters and the source's one are stamped onto their doors through the one
 * publisher, so the next RT counter is a call and not a fourth copy of the shape.
 * tests/reac-code-conformance.py keeps the shape here and nowhere else. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <spa/buffer/buffer.h>
#include <pipewire/properties.h>

/* The node property the counter is published under (a count of cycles, since start). */
#define REACPW_PROP_HEALTH_RT_BAD_BUFFERS "reac.health.rt-bad-buffers"

/* The node property the sink's dropped-frames counter is published under: the audio
 * frames the downstream ring refused, since start (reac_sink_node_dropped_frames). */
#define REACPW_PROP_HEALTH_RT_DROPPED_FRAMES "reac.health.rt-dropped-frames"

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

/* How a node stamps a dict onto its door: the sink's door update, the source's own
 * property merge. Called with the node the counter belongs to. */
typedef void (*reac_rt_counter_stamp_fn)(void *node, const struct spa_dict *dict);

/* MAIN LOOP, every tick: one RT-callback counter onto `node`'s door under `key`. Stamped
 * when it moves (and once at 0 on a fresh door), so a quiet node costs nothing. `last` is
 * what the door last showed (-1 = nothing yet); it is remembered only once the stamp was
 * handed over, so a stamp that could not be built is retried next tick. The caller has
 * already decided the node has a door to stamp. */
static inline void reac_rt_counter_publish(void *node, reac_rt_counter_stamp_fn stamp,
					   const _Atomic uint32_t *counter, int64_t *last,
					   const char *key)
{
	uint32_t v = atomic_load_explicit(counter, memory_order_relaxed);
	if (*last == (int64_t)v)
		return;
	char val[16];
	snprintf(val, sizeof val, "%u", v);
	struct pw_properties *props = pw_properties_new(key, val, NULL);
	if (!props)
		return;
	stamp(node, &props->dict);
	pw_properties_free(props);
	*last = (int64_t)v;
}

#endif
