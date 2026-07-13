// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_source_node.h"

#include <reac/reac.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/node/io.h>   /* struct spa_io_rate_match + SPA_IO_RateMatch */
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>

/* The largest quantum we ever expect from the graph. The scratch buffer (used
 * to swallow reads for unlinked ports) is sized to this; if the graph ever asks
 * for more, on_process clamps the read to it so we never write past it. */
#define REAC_MAX_QUANTUM 8192

/* One output port per REAC channel; pw_filter gives each port its own buffer
 * each cycle, which is exactly the planar layout the ring hands back. */
struct port { /* per-port user data (PipeWire stores the void* we hand it) */
	int channel;
	struct spa_io_rate_match *rate_match; /* set by io_changed; NULL until linked */
};

struct reac_source_node {
	struct pw_filter *filter;
	struct spa_hook listener;
	struct reac_ring *ring;
	struct reac_rx *rx;
	int sample_rate;
	int channels;
	int debug;   /* REAC_DEBUG env: emit per-second ring read peak/fill telemetry */
	struct port *ports[REAC_MAX_CHANNELS];
	float scratch[REAC_MAX_QUANTUM]; /* sink for unlinked ports; never read back */
};

/* REALTIME. Pull one quantum per channel from the ring into the port buffers,
 * then nudge the resampler ratio from the feeder's measured ppm error. */
static void on_process(void *data, struct spa_io_position *position)
{
	struct reac_source_node *n = data;
	uint32_t nframes = position->clock.duration;
	if (nframes > REAC_MAX_QUANTUM)
		nframes = REAC_MAX_QUANTUM; /* never write past a port/scratch buffer */

	/* The ring has ONE shared read cursor across all channels, so we must read
	 * every channel each cycle or the planes desync. Unlinked ports (no buffer
	 * this cycle) get pointed at the per-node scratch sink: their samples are
	 * read out of the ring and discarded, keeping all planes aligned and never
	 * dereferencing a NULL port buffer in the RT path. */
	float *dst[REAC_MAX_CHANNELS];
	int got_ports = 0;
	for (int c = 0; c < n->channels; c++) {
		/* Belt-and-braces: a NULL port (should never happen — the constructor now
		 * fails if add_port returns NULL) is treated like an unlinked buffer so the
		 * ring planes stay aligned and we never deref NULL in the RT path. */
		float *buf = n->ports[c] ? pw_filter_get_dsp_buffer(n->ports[c], nframes) : NULL;
		if (buf) {
			dst[c] = buf;
			got_ports++;
		} else {
			dst[c] = n->scratch; /* unlinked or NULL port: discard into scratch */
		}
	}
	if (got_ports == 0)
		return; /* nothing linked — leave the ring untouched for a real consumer */

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
	for (int c = 0; c < n->channels; c++) {
		if (n->ports[c] && n->ports[c]->rate_match) {
			n->ports[c]->rate_match->rate = rate;
			break;
		}
	}
}

/* PipeWire hands us the SPA_IO areas per port as links come and go. We stash the
 * SPA_IO_RateMatch area so on_process can publish the follower rate into it. */
static void on_io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	(void)data;
	if (!port_data)
		return; /* node-global io, not a port */
	struct port *p = port_data;
	if (id == SPA_IO_RateMatch)
		p->rate_match = (size >= sizeof(struct spa_io_rate_match)) ? area : NULL;
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
	.io_changed = on_io_changed,
};

struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *label)
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
	n->debug = getenv("REAC_DEBUG") != NULL;

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

	n->filter = pw_filter_new_simple(
		loop,
		"reac:capture",
		pw_properties_new(
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
			PW_KEY_NODE_RATE, rate_str,        /* advertise the recovered REAC rate */
			NULL),
		&filter_events, n);
	if (!n->filter) {
		free(n);
		return NULL;
	}

	/* Register one DSP (planar F32) output port per REAC channel. pw_filter DSP
	 * ports are mono float planar, which matches the ring exactly. */
	for (int c = 0; c < n->channels; c++) {
		char pname[24], achan[12];
		snprintf(pname, sizeof pname, "capture_%02d", c + 1);
		/* Each REAC input is a DISCRETE mono mic — mark it as an AUX channel so no
		 * graph tool (RaySession/qpwgraph) guesses FL/FR and pairs them as stereo.
		 * AUX0..AUXN is the standard designation for a multichannel device's
		 * independent mono ports. */
		snprintf(achan, sizeof achan, "AUX%d", c);
		n->ports[c] = pw_filter_add_port(
			n->filter,
			PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, pname,
				PW_KEY_AUDIO_CHANNEL, achan,
				NULL),
			NULL, 0);
		/* A NULL port would later be handed to pw_filter_get_dsp_buffer on the RT
		 * thread (SPA_CONTAINER_OF on NULL = a wild deref). Fail construction rather
		 * than return a live-but-broken node. */
		if (!n->ports[c]) {
			pw_filter_destroy(n->filter);
			free(n);
			return NULL;
		}
		n->ports[c]->channel = c;
	}

	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		pw_filter_destroy(n->filter);
		free(n);
		return NULL;
	}
	return n;
}

void reac_source_node_destroy(struct reac_source_node *n)
{
	if (!n)
		return;
	if (n->filter)
		pw_filter_destroy(n->filter);
	free(n);
}
