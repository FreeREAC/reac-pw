/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * A fake pw_stream buffer pool for the node process callbacks. Included by
 * test_reac_sink_rt_planes.c and test_reac_source_rt_planes.c AFTER they have
 * included the node's own .c (the callbacks are static) and BEFORE they use it.
 *
 * Every allocation is EXACTLY the size the buffer claims, so AddressSanitizer
 * reports the first byte a callback touches beyond what the buffer holds. */
#include <spa/buffer/buffer.h>

struct plane_spec {
	uint32_t alloc_floats;   /* what the data pointer really holds */
	uint32_t maxsize;        /* what the plane says it holds, bytes */
	uint32_t chunk_size;     /* the chunk's size, bytes (sink: valid input) */
	int data_null;           /* data pointer absent */
	int chunk_null;          /* chunk pointer absent */
};

/* What every plane of a good buffer carries (sink input), per plane: a level that
 * names the plane, so a plane that came through can be told from one that did not. */
#define FX_LEVEL(p) (0.25f * (float)((p) + 1))
static uint32_t fx_alloc[5];

static struct pw_buffer fx_pwb;
static struct spa_buffer fx_sbuf;
static int fx_queued;

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *s)
{
	(void)s;
	return &fx_pwb;
}

int pw_stream_queue_buffer(struct pw_stream *s, struct pw_buffer *b)
{
	(void)s; (void)b;
	fx_queued++;
	return 0;
}

/* The node property the main loop publishes the counter under: the last value a
 * pw_stream_update_properties call carried for it, and how many calls did. */
static char fx_prop_val[32];
static int fx_prop_calls;

int pw_stream_update_properties(struct pw_stream *s, const struct spa_dict *dict)
{
	(void)s;
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict)
		if (strcmp(it->key, REACPW_PROP_HEALTH_RT_BAD_BUFFERS) == 0) {
			snprintf(fx_prop_val, sizeof fx_prop_val, "%s", it->value);
			fx_prop_calls++;
		}
	return 0;
}

/* The published value is `want` (and has been published, not guessed). */
static int fx_published_is(uint32_t want)
{
	char w[32];
	snprintf(w, sizeof w, "%u", want);
	return fx_prop_calls > 0 && strcmp(fx_prop_val, w) == 0;
}

static void fx_build(uint32_t n_datas, const struct plane_spec *ps, uint64_t requested)
{
	memset(&fx_pwb, 0, sizeof fx_pwb);
	memset(&fx_sbuf, 0, sizeof fx_sbuf);
	fx_sbuf.n_datas = n_datas;
	fx_sbuf.datas = calloc(n_datas ? n_datas : 1, n_datas ? sizeof(struct spa_data) : 1);
	for (uint32_t i = 0; i < n_datas; i++) {
		struct spa_data *d = &fx_sbuf.datas[i];
		d->type = SPA_DATA_MemPtr;
		d->maxsize = ps[i].maxsize;
		d->data = ps[i].data_null ? NULL : calloc(ps[i].alloc_floats, sizeof(float));
		fx_alloc[i] = ps[i].data_null ? 0 : ps[i].alloc_floats;
		for (uint32_t k = 0; d->data && k < ps[i].alloc_floats; k++)
			((float *)d->data)[k] = FX_LEVEL(i);
		d->chunk = ps[i].chunk_null ? NULL : calloc(1, sizeof(struct spa_chunk));
		if (d->chunk)
			d->chunk->size = ps[i].chunk_size;
	}
	fx_pwb.buffer = &fx_sbuf;
	fx_pwb.requested = requested;
	fx_queued = 0;
}

#define FX_FLOATS 256u
#define FX_GOOD { FX_FLOATS, FX_FLOATS * 4, FX_FLOATS * 4, 0, 0 }

/* The named cases. Each is one process() cycle on one defective (or good) buffer. */
enum fx_case { FX_GOOD_BUF, FX_NO_PLANES, FX_FEW_PLANES, FX_NULL_DATA0, FX_NULL_DATA1,
               FX_SHORT_PLANE, FX_NO_CHUNK0, FX_NO_CHUNK1, FX_NULL_BUFFER, FX_NULL_DATAS,
               FX_SHORT_CHUNK, FX_EXTRA_NO_CHUNK, FX_N_CASES };
static const char *const fx_names[FX_N_CASES] = {
	"good", "no-planes", "few-planes", "null-data0", "null-data1", "short-plane", "no-chunk0", "no-chunk1",
	"null-buffer", "null-datas", "short-chunk", "extra-no-chunk"
};

/* The short plane's real allocation. The sink test leaves it EXACT, so a read past
 * it is an AddressSanitizer report. The source test sets FX_SHORT_SLACK: the source
 * writes through libreac-transport's ring read, which is not instrumented, so a
 * write past the plane would go unseen; the slack carries a canary instead and the
 * test fails if a single sample of it changed. */
#define FX_SHORT_FLOATS 64u
#define FX_SLACK_FLOATS 256u
#define FX_CANARY 0x7fc0dead
static int fx_short_slack;

static int fx_short_plane_clean(void)
{
	if (!fx_short_slack || fx_sbuf.n_datas < 2 || !fx_sbuf.datas)
		return 1;
	const uint32_t *p = (const uint32_t *)fx_sbuf.datas[1].data;
	for (uint32_t i = FX_SHORT_FLOATS; i < FX_SHORT_FLOATS + FX_SLACK_FLOATS; i++)
		if (p[i] != FX_CANARY)
			return 0;
	return 1;
}

/* Builds the case's buffer for a node of four channels. */
static void fx_build_case(enum fx_case c, uint64_t requested)
{
	struct plane_spec g[5] = { FX_GOOD, FX_GOOD, FX_GOOD, FX_GOOD, FX_GOOD };
	switch (c) {
	case FX_GOOD_BUF:   fx_build(4, g, requested); break;
	case FX_NO_PLANES:  fx_build(0, g, requested); break;
	case FX_FEW_PLANES: fx_build(2, g, requested); break;
	case FX_NULL_DATA0: g[0].data_null = 1; fx_build(4, g, requested); break;
	case FX_NULL_DATA1: g[1].data_null = 1; fx_build(4, g, requested); break;
	case FX_SHORT_PLANE:
		g[1].alloc_floats = FX_SHORT_FLOATS + (fx_short_slack ? FX_SLACK_FLOATS : 0);
		g[1].maxsize = FX_SHORT_FLOATS * 4; g[1].chunk_size = FX_SHORT_FLOATS * 4;
		fx_build(4, g, requested);
		if (fx_short_slack) {
			uint32_t *p = (uint32_t *)fx_sbuf.datas[1].data;
			for (uint32_t i = FX_SHORT_FLOATS; i < FX_SHORT_FLOATS + FX_SLACK_FLOATS; i++)
				p[i] = FX_CANARY;
		}
		break;
	case FX_NO_CHUNK0:  g[0].chunk_null = 1; fx_build(4, g, requested); break;
	case FX_NO_CHUNK1:  g[1].chunk_null = 1; fx_build(4, g, requested); break;
	case FX_NULL_BUFFER: fx_build(4, g, requested); fx_pwb.buffer = NULL; break;
	case FX_NULL_DATAS:  fx_build(4, g, requested); fx_sbuf.datas = NULL; break;
	/* plane 1 holds everything but its chunk says only 64 samples are valid: a
	 * bad INPUT plane (sink), a perfectly good OUTPUT plane (source) */
	case FX_SHORT_CHUNK: g[1].chunk_size = FX_SHORT_FLOATS * 4; fx_build(4, g, requested); break;
	/* a fifth plane beyond the node's four channels, with no chunk: the source
	 * stamps every plane's chunk, the sink never looks at it */
	case FX_EXTRA_NO_CHUNK: g[4].chunk_null = 1; fx_build(5, g, requested); break;
	default: break;
	}
}

/* WHAT A NODE MUST DO WITH EACH CASE. `sink` selects whose view it is.
 * A buffer with no planes array, or (sink) no chunk on plane 0, costs the whole
 * cycle. Anything else costs only the planes that are bad: the others play. */
static int fx_skip_whole(enum fx_case c, int sink)
{
	switch (c) {
	case FX_NO_PLANES: case FX_NULL_BUFFER: case FX_NULL_DATAS: return 1;
	case FX_NO_CHUNK0: return sink;
	default: return 0;
	}
}

/* Is plane p (of the node's four) one the node must carry? */
static int fx_plane_good(enum fx_case c, int p, int sink)
{
	if (fx_skip_whole(c, sink))
		return 0;
	switch (c) {
	case FX_FEW_PLANES:  return p < 2;
	case FX_NULL_DATA0:  return p != 0;
	case FX_NULL_DATA1:  return p != 1;
	case FX_SHORT_PLANE: return p != 1;
	case FX_NO_CHUNK0:   return p != 0;     /* source only: sink skips whole */
	case FX_NO_CHUNK1:   return p != 1;
	case FX_SHORT_CHUNK: return sink ? p != 1 : 1;
	default:             return 1;
	}
}

/* Is the cycle counted as having met a bad buffer? */
static int fx_counted(enum fx_case c, int sink)
{
	if (fx_skip_whole(c, sink))
		return 1;
	for (int p = 0; p < 4; p++)
		if (!fx_plane_good(c, p, sink))
			return 1;
	return 0;
}

static int fx_case_by_name(const char *s)
{
	for (int i = 0; i < FX_N_CASES; i++)
		if (strcmp(s, fx_names[i]) == 0)
			return i;
	return -1;
}
