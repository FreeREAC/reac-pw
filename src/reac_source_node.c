// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_source_node.h"
#include "reac_link_state.h"
#include "reac_node_ensure.h"  /* the shared same-box-or-rebuild decision */
#include "reac_sink_format.h"  /* the shared Format pod builder + renegotiate decision
                                 * (task #4.3 extension: "one wire, one rate" — see
                                 * reac_sink_format.h's revised SCOPE note) */

#include <reac/reac.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/node/io.h>   /* struct spa_io_rate_match + SPA_IO_RateMatch */
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>

/* The largest quantum we ever expect from the graph. The scratch buffer (used
 * to swallow reads for unlinked ports) is sized to this; if the graph ever asks
 * for more, on_process clamps the read to it so we never write past it. */
#define REAC_MAX_QUANTUM 8192

/* THIS NODE IS AN ADAPTER, NOT A RAW FILTER (2026-08-21). A pw_filter's ports are
 * raw DSP ports at the GRAPH rate with no audioconvert in the path, so a filter
 * could only ever run at the graph's pace — `node.rate` was a REQUEST for the graph
 * to switch, which an RME-driven 96 kHz graph refuses. The REAC pace and the rig
 * pace are independent (docs/design/specs/2026-08-21-reac-adapter-pace-and-port-
 * contract.md), so this is a pw_stream: it declares the REAC rate in its FORMAT and
 * PipeWire's own resampler bridges the two. Conversion happens here, in the adapter,
 * and nowhere else — the console is never asked to change pace for a box.
 *
 * The stream is F32 PLANAR with AUX0.. positions, which is the same planar layout
 * the ring hands back, so the RT path still writes straight into the buffer planes. */
struct reac_source_node {
	struct pw_stream *stream;
	struct spa_io_rate_match *rate_match; /* node-level; NULL until a resampler exists */
	struct spa_hook listener;
	struct reac_ring *ring;
	struct reac_rx *rx;
	int sample_rate;
	int channels;
	char label[64];   /* effective box label on node.description, "" = none (mirrors
	                   * reac_sink_node's own n->label — the ensure() identity check
	                   * needs it to tell a width-preserving relabel from a no-op) */
	int debug;   /* REAC_DEBUG env: emit per-second ring read peak/fill telemetry */
	float scratch[REAC_MAX_QUANTUM]; /* sink for absent planes; never read back */

	/* Set around source_reconnect_rate's pw_stream_disconnect/connect pair —
	 * the same belt-and-braces pattern as reac_sink_node.c's rate_reconnecting
	 * (see that field's comment for the full reasoning: on_process is not
	 * invoked by PipeWire on a disconnected stream, so this is a second, cheap
	 * line of defense on top of that guarantee, not the mechanism that makes
	 * a skipped cycle safe). MAIN-LOOP-only write; on_process (RT) reads it as
	 * the first check, relaxed load. */
	_Atomic int rate_reconnecting;
};

/* REALTIME. Pull one quantum per channel from the ring into the port buffers,
 * then nudge the resampler ratio from the feeder's measured ppm error. */
static void on_process(void *data)
{
	struct reac_source_node *n = data;
	/* Mid a rate reconnect (source_reconnect_rate): the stream is disconnected
	 * right now, so dequeuing a buffer below would hand back nothing anyway and
	 * this callback should not even be reached — PipeWire does not drive
	 * process() on a disconnected stream. Belt-and-braces documentation of that
	 * fact, not the mechanism that makes it true (see reac_sink_node.c's
	 * identical guard on rate_reconnecting for the full reasoning). A skipped
	 * cycle costs nothing dangerous: the ring keeps filling from the RX feeder
	 * independently of this stream's lifecycle and simply holds the frames for
	 * the next cycle once reconnected. */
	if (atomic_load_explicit(&n->rate_reconnecting, memory_order_relaxed))
		return;
	struct pw_buffer *pwb = pw_stream_dequeue_buffer(n->stream);
	if (!pwb)
		return;                 /* no buffer this cycle — leave the ring untouched */
	struct spa_buffer *buf = pwb->buffer;
	/* How many frames the consumer wants, bounded by what the plane can hold. A
	 * stream buffer is sized by the ADAPTER at OUR rate, so this is already the
	 * REAC-pace frame count — PipeWire resamples it to the graph on the way out. */
	uint32_t cap = buf->datas[0].maxsize / (uint32_t)sizeof(float);
	uint32_t nframes = pwb->requested ? (uint32_t)pwb->requested : cap;
	if (nframes > cap)
		nframes = cap;
	if (nframes > REAC_MAX_QUANTUM)
		nframes = REAC_MAX_QUANTUM; /* never write past a plane/scratch buffer */

	/* The ring has ONE shared read cursor across all channels, so we must read
	 * every channel each cycle or the planes desync. Unlinked ports (no buffer
	 * this cycle) get pointed at the per-node scratch sink: their samples are
	 * read out of the ring and discarded, keeping all planes aligned and never
	 * dereferencing a NULL port buffer in the RT path. */
	float *dst[REAC_MAX_CHANNELS];
	int got_ports = 0;
	for (int c = 0; c < n->channels; c++) {
		/* A plane the adapter did not give us is discarded into scratch, so the
		 * ring's ONE shared read cursor stays aligned across channels and the RT
		 * path never dereferences NULL. */
		float *plane = ((uint32_t)c < buf->n_datas) ? buf->datas[c].data : NULL;
		if (plane) {
			dst[c] = plane;
			got_ports++;
		} else {
			dst[c] = n->scratch;
		}
	}
	if (got_ports == 0) {
		pw_stream_queue_buffer(n->stream, pwb);
		return;                 /* no planes — leave the ring for a real consumer */
	}

	/* Bound latency + keep audio fresh: if the producer over-filled (it paces to
	 * the wire and can outrun a just-started/quantum-bursty consumer), drop the
	 * oldest excess down to a few quanta. Without this the ring pegs full and the
	 * producer's drop-newest chops the stream once per cycle (a quantum-rate buzz).
	 * SPSC-safe — the consumer owns tail. The clock loop handles fine drift; this
	 * only catches gross over-fill. */
	reac_ring_trim(n->ring, nframes * 4);

	reac_ring_read_planar(n->ring, dst, n->channels, nframes);

	/* Opt-in telemetry (REAC_DEBUG): publish the last ring-read peak/fill into the
	 * rx diagnostics for the NON-RT feeder thread to print — no fprintf on the RT
	 * path. Distinguishes "ring starved" (peak ~0, fill < quantum = producer/graph
	 * clock drift, #131) from "read fine, consumer wrong". */
	if (n->debug) {
		float peak = 0.0f; int active = 0;
		for (int ch = 0; ch < n->channels; ch++) {
			if (dst[ch] == n->scratch) continue;
			float m = 0.0f;
			for (uint32_t s = 0; s < nframes; s++) {
				float a = dst[ch][s] < 0 ? -dst[ch][s] : dst[ch][s];
				if (a > m) m = a;
			}
			if (m > 1e-6f) active++;
			if (m > peak) peak = m;
		}
		atomic_store_explicit(&n->rx->src_peak_micro, (int)(peak * 1e6f), memory_order_relaxed);
		atomic_store_explicit(&n->rx->src_active_ch, active, memory_order_relaxed);
		atomic_store_explicit(&n->rx->src_fill, (int)reac_ring_readable(n->ring), memory_order_relaxed);
	}

	/* FOLLOWER drift correction (Tier-A clock bridge). io_rate_match.rate is the
	 * ratio PipeWire's async resampler applies to OUR output on each link; the
	 * graph clock is the reference and we are the follower. The feeder tracks the
	 * byte-14/15 counter slope vs CLOCK_MONOTONIC into a slowly-filtered ppm error
	 * (locked to the long-term slope, not packet jitter) — we convert that to a
	 * rate correction so the resampler chases the desk's TRUE rate. One rate_match
	 * area is shared across the port group, so the first linked port carries it. */
	int ppm_milli = atomic_load_explicit(&n->rx->ppm_error_milli, memory_order_relaxed);
	double rate = 1.0 + (double)ppm_milli / 1e9; /* ppm*1000 -> fractional */
	if (n->rate_match)
		n->rate_match->rate = rate;

	/* Tell the adapter how much of each plane we filled. Every plane carries the
	 * same frame count — the ring's read is planar and aligned by construction. */
	for (uint32_t c = 0; c < buf->n_datas; c++) {
		buf->datas[c].chunk->offset = 0;
		buf->datas[c].chunk->stride = (int32_t)sizeof(float);
		buf->datas[c].chunk->size = nframes * (uint32_t)sizeof(float);
	}
	pw_stream_queue_buffer(n->stream, pwb);
}

/* PipeWire hands us the SPA_IO areas as the stream is configured. We stash the
 * SPA_IO_RateMatch area so on_process can publish the follower rate into it — the
 * adapter's resampler is what reads it, which is exactly the drift correction the
 * filter version did, now driving OUR OWN resampler instead of a peer's. */
static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct reac_source_node *n = data;
	if (id == SPA_IO_RateMatch)
		n->rate_match = (size >= sizeof(struct spa_io_rate_match)) ? area : NULL;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.io_changed = on_io_changed,
};

struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *label,
                                              int master_role)
{
	struct reac_source_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->ring = ring;
	n->rx = rx;
	n->sample_rate = sample_rate;
	/* Expose the box's real input width; fall back to the full fabric. */
	n->channels = (channels > 0 && channels <= REAC_MAX_CHANNELS)
	              ? channels : REAC_MAX_CHANNELS;
	snprintf(n->label, sizeof n->label, "%s", label ? label : "");
	n->debug = getenv("REAC_DEBUG") != NULL;
	atomic_init(&n->rate_reconnecting, 0);

	char rate_str[16];
	snprintf(rate_str, sizeof rate_str, "1/%d", sample_rate);

	/* Per-instance node name so one master per REAC VLAN/segment coexists. */
	char nodename[64];
	if (inst && *inst)
		snprintf(nodename, sizeof nodename, "reac-capture.%s", inst);
	else
		snprintf(nodename, sizeof nodename, "reac-capture");
	char desc[128];
	if (label && *label)
		snprintf(desc, sizeof desc, "%s — %d ch (REAC box inputs)", label, n->channels);
	else
		snprintf(desc, sizeof desc, "REAC %dch capture (%s)", n->channels,
		         rx && rx->cfg.accept == REAC_RX_ACCEPT_UPSTREAM
		           ? "box mic inputs" : "master downstream");

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture",  /* a source produces audio */
		PW_KEY_MEDIA_CLASS, "Audio/Source",
		PW_KEY_MEDIA_ROLE, "Production",
		PW_KEY_NODE_NAME, nodename,
		PW_KEY_NODE_DESCRIPTION, desc,
		/* Follower (default): the DAC/PHC drives the graph and PipeWire
		 * async-resamples our REAC clock into it. To make REAC the graph
		 * DRIVER instead, add PW_KEY_NODE_DRIVER "true" + a clock rate and
		 * register a clock source — see NATIVE-REAC-DESIGN.md Section 3.4. */
		/* NO node.rate. On a filter that was a REQUEST for the graph to run at the
		 * REAC rate, which an RME-driven graph refuses; the rate that matters is the
		 * one in our FORMAT below, which the adapter resamples from. */
		NULL);
	/* CREATE-TIME-ONLY badge props (task #154), master role only — see the
	 * header doc for why: no live update here (this node has no pacer handle),
	 * and the slave-role link state is a different FSM entirely. */
	if (props && master_role) {
		pw_properties_set(props, REAC_PROP_LINK_STATE,
		                  reac_link_state_name(REAC_LINK_PROBING));
		pw_properties_set(props, REAC_PROP_BOX_MODEL, "none");
		pw_properties_set(props, REAC_PROP_BOX_WIDTH, "0x0");
	}

	n->stream = pw_stream_new_simple(loop, "reac:capture", props, &stream_events, n);
	if (!n->stream) {
		free(n);
		return NULL;
	}

	/* ONE format, not one port per channel: the adapter builds the ports from it.
	 * F32 PLANAR keeps the ring's layout, and AUX0..AUXN marks each REAC input as a
	 * DISCRETE mono mic so no graph tool guesses FL/FR and pairs them as stereo.
	 * PipeWire names the resulting ports capture_AUX0.. — which is why the physical
	 * input a port carries is DECLARED in openmixer's contract rather than parsed
	 * out of the name (same spec). Built by reac_sink_format_build (shared with
	 * reac:playback) so a later renegotiation (source_reconnect_rate) produces
	 * the exact same pod shape as this initial connect, rather than a second
	 * hand-copied one — the same reasoning reac_sink_node.c's sink_open_filter
	 * already documents for the sink side. */
	uint8_t fbuf[1024];
	struct spa_pod_builder fb = SPA_POD_BUILDER_INIT(fbuf, sizeof fbuf);
	const struct spa_pod *params[1] = {
		reac_sink_format_build(&fb, n->channels, n->sample_rate),
	};

	if (pw_stream_connect(n->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
	                      params, 1) < 0) {
		pw_stream_destroy(n->stream);
		free(n);
		return NULL;
	}
	return n;
}

void reac_source_node_destroy(struct reac_source_node *n)
{
	if (!n)
		return;
	if (n->stream) {
		/* Explicit disconnect before destroy — see reac_sink_node.c's ensure()
		 * comment for why this ordering matters when a caller (reac_source_node_
		 * ensure below) immediately queues a replacement stream's connect in the
		 * same call: the two requests must reach the daemon in the order sent. */
		pw_stream_disconnect(n->stream);
		pw_stream_destroy(n->stream);
	}
	free(n);
}

/* See the header: the sink's main-loop timer drives this so the capture badge follows
 * the box. pw_stream_update_properties MERGES — only the keys we set change; the ports,
 * rate, media.* seeded at create persist untouched. A NULL arg skips that key. */
void reac_source_node_publish_link(struct reac_source_node *n,
                                   const char *link_state,
                                   const char *box_model,
                                   const char *box_width)
{
	if (!n || !n->stream)
		return;
	struct pw_properties *props = pw_properties_new(NULL, NULL);
	if (!props)
		return;
	if (link_state)
		pw_properties_set(props, REAC_PROP_LINK_STATE, link_state);
	if (box_model)
		pw_properties_set(props, REAC_PROP_BOX_MODEL, box_model);
	if (box_width)
		pw_properties_set(props, REAC_PROP_BOX_WIDTH, box_width);
	pw_stream_update_properties(n->stream, &props->dict);
	pw_properties_free(props);
}

/* MAIN LOOP: force the live adapter to actually present `hz`, the reac-
 * capture mirror of reac_sink_node.c's sink_reconnect_rate — see that
 * function's comment for the full reasoning (a bare pw_stream_update_params
 * advertises a new supported set but never renegotiates the ACTIVE format;
 * the robust trigger is a same-object disconnect + connect at a fresh Format
 * pod). Same-stream-object, not a destroy+recreate, for the same reason: a
 * rate change is not a box change, so the node identity (name, description,
 * badge props seeded at create) must not flash back to a connect-time seed.
 *
 * RT FEED SAFETY mirrors the sink exactly: on_process runs on the stream's
 * own RT data thread under PW_STREAM_FLAG_RT_PROCESS, PipeWire does not
 * invoke it while disconnected, and rate_reconnecting is the belt-and-braces
 * second check on top of that guarantee. A skipped cycle costs nothing
 * dangerous — the ring keeps filling independently of this stream and simply
 * holds the frames for the next cycle.
 *
 * Adopts `hz` only on a successful reconnect (reac_sink_format_rate_after_
 * attempt, shared with the sink); on failure it re-asserts the OLD rate so
 * the node is not left silently disconnected, and returns -1. */
static int source_reconnect_rate(struct reac_source_node *n, int hz)
{
	if (!n->stream)
		return -1;
	int prev_hz = n->sample_rate;

	atomic_store_explicit(&n->rate_reconnecting, 1, memory_order_relaxed);
	pw_stream_disconnect(n->stream);
	n->rate_match = NULL;   /* a fresh connect gets a fresh (possibly different)
	                         * SPA_IO_RateMatch area; the old pointer is stale
	                         * until on_io_changed fires again. */

	uint8_t fbuf[1024];
	struct spa_pod_builder fb = SPA_POD_BUILDER_INIT(fbuf, sizeof fbuf);
	const struct spa_pod *params[1] = {
		reac_sink_format_build(&fb, n->channels, hz),
	};

	int ok = pw_stream_connect(n->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                          PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
	                          params, 1) >= 0;
	if (!ok) {
		pw_log_warn("reac:capture — rate reconnect to %d Hz failed; "
		            "re-asserting %d Hz so the source is not left disconnected",
		            hz, prev_hz);
		uint8_t fbuf2[1024];
		struct spa_pod_builder fb2 = SPA_POD_BUILDER_INIT(fbuf2, sizeof fbuf2);
		const struct spa_pod *fallback[1] = {
			reac_sink_format_build(&fb2, n->channels, prev_hz),
		};
		if (pw_stream_connect(n->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
		                      fallback, 1) < 0)
			pw_log_warn("reac:capture — fallback reconnect to %d Hz ALSO failed; "
			            "source is disconnected until the next box/format event", prev_hz);
	}
	atomic_store_explicit(&n->rate_reconnecting, 0, memory_order_relaxed);

	n->sample_rate = reac_sink_format_rate_after_attempt(hz, prev_hz, ok);
	return ok ? 0 : -1;
}

void reac_source_node_publish_rate(struct reac_source_node *n, int hz)
{
	if (!n || !n->stream)
		return;
	/* False on a byte-identical poll (hz already matches what this node
	 * presents) and on a non-positive hz (never a real accepted rate) — same
	 * pure decision the sink already relies on, so a normal single-rate boot
	 * or an already-caught-up node never reconnects at all. */
	if (reac_sink_format_needs_update(n->sample_rate, hz))
		source_reconnect_rate(n, hz);
}

int reac_source_node_ensure(struct reac_source_node **slot,
                            const struct reac_source_node_cfg *cfg,
                            int channels, const char *label)
{
	if (!slot || !cfg)
		return -1;
	/* Normalise to the same width reac_source_node_new would settle on, so the
	 * "same box?" test compares like with like (a startup channels=0 becomes 40).
	 * A REAC box input width is USUALLY model-unique (8=S-0808, 16=S-1608,
	 * 32=S-4000S), but width alone is not the whole identity: reac_sink_node_
	 * ensure has always also compared the LABEL (a same-width swap to a
	 * differently-labelled box, or a re-enrolled pin with a new operator label,
	 * is still a real change) — this one drifted to width-only, so a label-only
	 * change left reac-capture's node.description/badges stamped with the OLD
	 * box's identity forever (reac_node_ensure.h's own header documents this as
	 * the bug the shared decision fixes). want_label mirrors reac_sink_node_
	 * ensure's own want_label seeding (label ? label : ""). */
	int want = (channels > 0 && channels <= REAC_MAX_CHANNELS) ? channels
	                                                           : REAC_MAX_CHANNELS;
	char want_label[64];
	snprintf(want_label, sizeof want_label, "%s", label ? label : "");
	struct reac_source_node *cur = *slot;
	if (!reac_node_ensure_needs_rebuild(cur != NULL, cur ? cur->channels : 0,
	                                    cur ? cur->label : NULL, want, want_label))
		return 0;
	/* Absent, or a real change (a live box swap, or a same-width relabel): the old
	 * box's identity no longer applies, so tear the stale node down first, then
	 * build fresh at the new width/label via the unchanged create API. The RX ring
	 * is shared + unchanged, so the new node reads the same planes the feeder keeps
	 * filling. */
	if (cur) {
		reac_source_node_destroy(cur);
		*slot = NULL;
	}
	*slot = reac_source_node_new(cfg->loop, cfg->ring, cfg->rx, cfg->sample_rate,
	                             want, cfg->inst, label, cfg->master_role);
	return *slot ? 0 : -1;
}
