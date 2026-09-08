// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_source_node.h"
#include "reac_link_state.h"
#include "reac_node_ensure.h"  /* the shared same-box-or-rebuild decision */
#include "reac_sink_format.h"  /* the shared Format pod builder + renegotiate decision
                                 * (task #4.3 extension: "one wire, one rate" — see
                                 * reac_sink_format.h's revised SCOPE note) */
#include "reac_role_cfg.h"    /* the `reac.cfg.role` parse + refusal codes */
#include "reac_rate_cfg.h"    /* REAC_PROP_RATE — the wire pace this segment locked to */
#include "reac_segment_ident.h" /* the segment identity + the slave answer set */
#include "reac_role_swap.h"   /* the swap's lifecycle answer (arbitration §8) */

#include <reac/reac.h>
#include <spa/param/param.h>   /* SPA_PARAM_Props — the role write door */
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/node/io.h>   /* struct spa_io_rate_match + SPA_IO_RateMatch */
#include "reac_pacer.h"   /* the segment's clock discipline: the graph-clock door */
#include "reac_clock.h"   /* reac_clock_name_is_hardware, for the REAC_DEBUG line */

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
	char nodename[80];              /* what this node is called, for its own messages */
	/* The graph-clock reference, published from the RT callback into the segment's
	 * pacer. See reac_source_node_cfg's own comment for why this node has it too. */
	struct reac_pacer *pacer;
	const char *clock_ref;
	struct spa_io_position *position;   /* SPA_IO_Position area; NULL until configured */
	char graph_clock[64];               /* last driver clock name seen (REAC_DEBUG line) */
	/* One diagnostic line, composed by the RT callback and printed by the main loop.
	 * The flag is the whole handshake: RT writes the buffer only while it is 0 and then
	 * sets it; main reads only while it is 1 and then clears it. */
	_Atomic int log_pending;
	char log_line[160];
	float scratch[REAC_MAX_QUANTUM]; /* sink for absent planes; never read back */

	/* Set around source_reconnect_rate's pw_stream_disconnect/connect pair —
	 * the same belt-and-braces pattern as reac_sink_node.c's rate_reconnecting
	 * (see that field's comment for the full reasoning: on_process is not
	 * invoked by PipeWire on a disconnected stream, so this is a second, cheap
	 * line of defense on top of that guarantee, not the mechanism that makes
	 * a skipped cycle safe). MAIN-LOOP-only write; on_process (RT) reads it as
	 * the first check, relaxed load. */
	_Atomic int rate_reconnecting;

	/* THE SLAVE ROLE'S ONLY DOOR. A slave has no reac-playback node — main.c's
	 * listener_open builds reac_sink_node for the master branch alone — so this
	 * capture node is the one place a `reac.cfg.role` assertion can reach a
	 * recorder, and the one place its answer can be published. The record is the
	 * listener's (reac_role_swap.h); NULL when none is wired, and then this door
	 * is inert and reads nothing.
	 *
	 * WHAT THE CONSOLE CAN AND CANNOT REACH THROUGH IT: openmixer addresses a
	 * segment by its `reac-playback[.<inst>]` node, so it can drive a swap TO
	 * the recorder end but cannot yet drive one back — the return door needs the
	 * slave-side playback node that docs/SLAVE-EMULATION-SCOPE.md's W1 owns.
	 * The mechanism below is complete and reversible; its console reach is not,
	 * and nothing here pretends otherwise. */
	struct reac_role_swap *role_swap;
	_Atomic int reopen_role;   /* accepted reac.cfg.role awaiting main's clean
	                            * segment re-open, as role+1 (0 = none) */
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
	/* THE GRAPH-CLOCK SAMPLE, taken BEFORE any early return, because it is a fact
	 * about the graph and not about this cycle's buffer: a cycle with nothing to
	 * dequeue still had a driver, and that driver is the reference we are grading.
	 * Inert unless the segment has a pacer and it is following (reac_pacer.h). */
	if (n->pacer && n->position) {
		const struct spa_io_clock *c = &n->position->clock;
		reac_pacer_clock_publish_graph(n->pacer, c->name,
		                               (c->flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) != 0,
		                               c->rate_diff, c->nsec, n->clock_ref);
		/* REAC_DEBUG only, and only when the DRIVER CHANGES: which clock is driving
		 * this node is the one fact that decides whether a reference exists at all,
		 * and it was previously invisible — the sink published the same sample from a
		 * callback that a suspended node never runs, so "no reference" and "nobody
		 * asked" read identically. One bounded string compare per cycle, a line per
		 * change, exactly like this node's existing ring telemetry. */
		/* THE RT CALLBACK DOES NOT PRINT (this file's own law, and reac_pacer.h's).
		 * The line is COMPOSED here into a fixed buffer — bounded, no allocation, no
		 * syscall — and handed to the main loop through one atomic flag, which is the
		 * same shape as the pacer's event ring in miniature. One line per driver
		 * change, only under REAC_DEBUG, and dropped rather than queued twice if the
		 * main loop has not drained the last one. */
		if (n->debug && strncmp(n->graph_clock, c->name, sizeof n->graph_clock - 1) != 0 &&
		    atomic_load_explicit(&n->log_pending, memory_order_acquire) == 0) {
			snprintf(n->graph_clock, sizeof n->graph_clock, "%s", c->name);
			snprintf(n->log_line, sizeof n->log_line,
			         "reac-pw: %s: graph clock = %s (%s)\n", n->nodename,
			         c->name[0] ? c->name : "(unnamed)",
			         reac_clock_name_is_hardware(c->name) ? "a hardware clock"
			                                              : "refused: not a hardware clock");
			atomic_store_explicit(&n->log_pending, 1, memory_order_release);
		}
	}
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
	if (id == SPA_IO_Position)
		n->position = (size >= sizeof(struct spa_io_position)) ? area : NULL;
	if (id == SPA_IO_RateMatch)
		n->rate_match = (size >= sizeof(struct spa_io_rate_match)) ? area : NULL;
}

/* MAIN LOOP. The Props write door — the slave role's only one (see the struct's
 * role_swap comment). A `reac.cfg.role` assertion is DECIDED by the pure core
 * and FILED against the segment's record; carrying it out is main's poll timer,
 * as a clean listener re-open in the other engine, exactly as the master's own
 * door does it. Nothing here touches the RT path.
 *
 * A malformed value is refused and changes nothing — the running role is
 * untouched, so its answer must not move either. */
static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct reac_source_node *n = data;
	if (id != SPA_PARAM_Props || !param || !n->role_swap)
		return;

	enum reac_role req_role;
	int parsed = reac_role_prop_parse(param, &req_role);
	if (parsed <= 0)
		return;                  /* absent, or unusable: nothing asserted here */
	if (reac_role_swap_request(n->role_swap, req_role))
		atomic_store_explicit(&n->reopen_role, (int)req_role + 1, memory_order_relaxed);
}

/* MAIN LOOP. A NODE THAT DID NOT APPEAR HAS TO SAY SO.
 *
 * There was no state handler here at all, so a stream that errored on connect — or one
 * PipeWire refused for any reason — left the daemon reporting "autodetected S-1608 ->
 * reac-capture 16 in" over a graph that held no such node, and nothing anywhere said
 * otherwise. That is the shape of a silent failure this project refuses: nine minutes of
 * dead input patches on 2026-09-08 with a journal that read like success. ERROR is
 * printed with PipeWire's own reason; the other transitions are debug-only, because a
 * healthy stream walks through several of them on every rebuild. */
static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
	struct reac_source_node *n = data;
	if (state == PW_STREAM_STATE_ERROR)
		fprintf(stderr, "reac-pw: %s: stream ERROR — %s (this node is NOT in the graph; "
		        "its patches cannot exist)\n", n->nodename, error ? error : "no reason given");
	else if (n->debug)
		fprintf(stderr, "reac-pw: %s: stream %s -> %s\n", n->nodename,
		        pw_stream_state_as_string(old), pw_stream_state_as_string(state));
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
	.io_changed = on_io_changed,
	.param_changed = on_param_changed,
};

struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *label,
                                              int master_role,
                                              struct reac_pacer *pacer,
                                              const char *clock_ref)
{
	struct reac_source_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	/* BEFORE pw_stream_connect, which can have the RT callback running by the time
	 * this function returns: a field the RT path reads is never filled by the caller
	 * afterwards. */
	n->pacer = pacer;
	n->clock_ref = clock_ref;
	atomic_init(&n->log_pending, 0);
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

	/* Per-instance node name so one master per REAC VLAN/segment coexists. Kept on the
	 * node so its own failures can name themselves (on_state_changed). */
	char *nodename = n->nodename;
	if (inst && *inst)
		snprintf(n->nodename, sizeof n->nodename, "reac-capture.%s", inst);
	else
		snprintf(n->nodename, sizeof n->nodename, "reac-capture");
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
		pw_properties_set(props, REAC_PROP_BOX_MAC, REAC_BOX_MAC_NONE);
	}
	/* THE SEGMENT'S IDENTITY, ON BOTH NODES OF THE PAIR. It used to be published on
	 * the slave's capture node and on the master's playback node only — "exactly one
	 * node per segment answers to it" — and that reading cost the console its grip on
	 * a box: it keys a stagebox off the reac.* identity of the nodes it finds, and a
	 * capture node whose reac.segment read `null` (measured on the rig 2026-09-08,
	 * reac-capture.enp131s0) belongs to no segment as far as any client can tell.
	 * A segment's two nodes are two halves of ONE thing and carry the same identity;
	 * which node is the WRITE door is a different question, and role_swap still
	 * answers it (see the role_swap field's comment). */
	if (props)
		pw_properties_set(props, REAC_PROP_SEGMENT, reac_segment_name(inst));

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
/* The reac_prop_set_fn adapter, as on the sink: the composer writes through this
 * so a test can drive the same call with a recording fake. */
static void source_prop_set(void *ctx, const char *key, const char *value)
{
	pw_properties_set(ctx, key, value);
}

void reac_source_node_publish_link(struct reac_source_node *n,
                                   const char *link_state,
                                   const char *box_model,
                                   const char *box_width,
                                   uint64_t box_mac48)
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
	/* Unconditional, unlike the three above: 0 is a MEANING here (no box) and not
	 * "leave it alone", and a merge that skipped it would keep the departed box's
	 * address on the capture node while the playback node had already cleared it. */
	reac_box_mac_publish(box_mac48, source_prop_set, props);
	pw_stream_update_properties(n->stream, &props->dict);
	pw_properties_free(props);
}

/* MAIN LOOP: the SLAVE segment's whole published answer, in one update. In the
 * MASTER role the sink publishes all of this on reac-playback and this is never
 * called; in the SLAVE role there is no sink node at all, so this is the only
 * place the segment answers for itself.
 *
 * ONE UPDATE, NOT TWO. The role trio and the reac.master.* aggregate move
 * together — the hunt ending is simultaneously `applied` and a master appearing —
 * and a reader that caught one without the other would compute a disagreement
 * that never existed. Same MERGE semantics as publish_link above: only the keys
 * set here change, and the create-time identity/ports/format persist untouched.
 *
 * WHAT IS NOT HERE IS NOT AN OMISSION. A slave publishes no reac.discovery.*
 * (it runs no disco classifier), no reac.link-state / reac.box-* (a different
 * FSM), no reac.rate.drivable (it drives no pace) and no reac.headamp.* (a box
 * is told what its preamps do). Each absence is a fact a consumer reads as one;
 * a default would be a claim. */
void reac_source_node_publish_segment(struct reac_source_node *n,
                                      const char *role,
                                      const char *state,
                                      const char *refused,
                                      const struct reac_segment_answer *answer)
{
	if (!n || !n->stream)
		return;
	struct pw_properties *props = pw_properties_new(NULL, NULL);
	if (!props)
		return;
	if (role)
		pw_properties_set(props, REAC_PROP_ROLE, role);
	if (state)
		pw_properties_set(props, REAC_PROP_ROLE_STATE, state);
	if (refused)
		pw_properties_set(props, REAC_PROP_ROLE_REFUSED, refused);
	if (answer) {
		pw_properties_set(props, REAC_PROP_MASTER_STATE, answer->master_state);
		pw_properties_set(props, REAC_PROP_MASTER_MAC, answer->master_mac);
		pw_properties_set(props, REAC_PROP_PACE_SOURCE, answer->pace_source);
		pw_properties_set(props, REAC_PROP_MASTER_CONFLICT, answer->conflict);
		pw_properties_set(props, REAC_PROP_RIVAL_KIND, answer->rival_kind);
		pw_properties_set(props, REAC_PROP_REFUSAL, answer->refusal);
		pw_properties_set(props, REAC_PROP_RATE, answer->rate);
	}
	pw_stream_update_properties(n->stream, &props->dict);
	pw_properties_free(props);
}

/* Wire the SEGMENT's role lifecycle record — see the struct's own comment for
 * why the slave's door lives on this node. NULL detaches. */
void reac_source_node_set_role_swap(struct reac_source_node *n, struct reac_role_swap *swap)
{
	if (n)
		n->role_swap = swap;
}

/* Take (read+clear) the pending accepted reac.cfg.role for a clean listener
 * re-open in the other engine, or -1 if none. Main's poll timer calls this —
 * the capture-node twin of reac_sink_node_take_reopen_role. */
int reac_source_node_take_reopen_role(struct reac_source_node *n)
{
	int r = n ? atomic_exchange_explicit(&n->reopen_role, 0, memory_order_relaxed) : 0;
	return r ? r - 1 : -1;
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

void reac_source_node_drain_log(struct reac_source_node *n, FILE *out)
{
	if (!n || !out)
		return;
	if (atomic_load_explicit(&n->log_pending, memory_order_acquire) != 1)
		return;
	fputs(n->log_line, out);
	atomic_store_explicit(&n->log_pending, 0, memory_order_release);
}

int reac_source_node_on_graph(const struct reac_source_node *n, const char **why)
{
	const char *reason = "no node was ever created";
	if (n && n->stream) {
		const char *err = NULL;
		enum pw_stream_state st = pw_stream_get_state(n->stream, &err);
		uint32_t id = pw_stream_get_node_id(n->stream);
		if (st == PW_STREAM_STATE_ERROR)
			reason = err ? err : "the stream is in error";
		else if (id == SPA_ID_INVALID)
			reason = "the daemon has given it no node id";
		else {
			if (why)
				*why = "on the graph";
			return 1;
		}
	}
	if (why)
		*why = reason;
	return 0;
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
	/* The clock door travels with every rebuild — a resized node is a NEW stream, and a
	 * graph-clock sample that stopped at the first box swap would be a reference that
	 * quietly disappeared — and it is set INSIDE the constructor, before the connect. */
	*slot = reac_source_node_new(cfg->loop, cfg->ring, cfg->rx, cfg->sample_rate,
	                             want, cfg->inst, label, cfg->master_role,
	                             cfg->pacer, cfg->clock_ref);
	return *slot ? 0 : -1;
}
