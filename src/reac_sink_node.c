// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac:playback — Audio/Sink that encodes the graph's PCM into REAC downstream
 * frames and hands them to the SCHED_FIFO cadence pacer (reac_pacer), which is
 * the REAC MASTER:
 *
 *   - registers an Audio/Sink with `channels` mono DSP input ports, so apps
 *     (Rhythmbox, pw-play, ...) and the graph can play INTO it;
 *   - process() de-stages each quantum into 12-sample REAC frames, encodes them
 *     with reac_tx_build, and SUBMITS them to the pacer's TX ring (a lock-free
 *     non-blocking push — NO syscall on the RT graph thread);
 *   - the pacer thread emits frames at a rock-steady pps (8000/4000/3675) and
 *     stamps the master JOIN/HOLD control sequence (probe -> grant -> established
 *     channel-map + cfea announce) onto the downstream broadcast, so a real
 *     Roland stagebox slaves to us. See NATIVE-REAC-DESIGN.md §2 (master
 *     handshake) + §6 (the pacer).
 *
 * The earlier first cut emitted a zero-control-block FILLER straight from
 * process() via reac_tx_emit (no handshake, sendto on the RT thread). A real
 * desk does not link to FILLER and clicks on graph-thread cadence jitter; the
 * pacer + master FSM close both gaps. Loopback (reac:playback -> reac:capture)
 * still works: the established stream carries audio FILLER our reac:capture
 * decodes, with the cdea/cfea control frames interspersed ~1/s. */

#include "reac_sink_node.h"
#include "reac_tx.h"
#include "reac_pacer.h"

#include <reac/reac.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <stdlib.h>
#include <string.h>

struct port_in { /* per-port user data PipeWire hands back */
	int channel;
};

struct reac_sink_node {
	struct pw_filter *filter;
	struct reac_pacer pacer;
	int pacer_open;
	int channels;
	int sample_rate;
	uint8_t src[6];           /* our master MAC */
	struct port_in *ports[REAC_MAX_CHANNELS];

	/* 12-sample-per-channel staging accumulator: a PipeWire quantum is not a
	 * multiple of REAC_SAMPLES_PER_PKT, so we carry the remainder across cycles
	 * and emit a frame each time the stage fills. */
	float stage[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	int staged; /* 0..REAC_SAMPLES_PER_PKT-1 */
};

/* REALTIME. Pull this quantum's PCM from the input ports, accumulate into the
 * 12-sample stage, encode a REAC frame for every full group, and SUBMIT it to the
 * pacer's TX ring. No syscall here: the lock-free push hands the frame to the
 * SCHED_FIFO pacer thread, which clocks the wire at a fixed pps. The counter and
 * the master control block are NOT stamped here — the pacer owns the master FSM
 * and stamps them on egress, so the cadence + handshake stay authoritative even
 * across a graph stall. */
static void on_process(void *data, struct spa_io_position *position)
{
	struct reac_sink_node *n = data;
	if (!n->pacer_open)
		return;
	uint32_t nframes = position->clock.duration;

	const float *in[REAC_MAX_CHANNELS];
	int have = 0;
	for (int c = 0; c < n->channels; c++) {
		float *b = pw_filter_get_dsp_buffer(n->ports[c], nframes);
		in[c] = b;            /* NULL if this port is unlinked this cycle */
		if (b)
			have++;
	}
	if (have == 0)
		return;               /* nothing feeding us — pacer free-runs silent FILLER */

	float *planar[REAC_MAX_CHANNELS];
	for (int c = 0; c < n->channels; c++)
		planar[c] = n->stage[c];

	uint8_t frame[REAC_FRAME_BYTES];
	for (uint32_t s = 0; s < nframes; s++) {
		for (int c = 0; c < n->channels; c++)
			n->stage[c][n->staged] = in[c] ? in[c][s] : 0.0f;
		if (++n->staged == REAC_SAMPLES_PER_PKT) {
			/* Encode audio + L2 header; counter/control are stamped by the pacer.
			 * Counter 0 is a placeholder (overwritten on egress). */
			reac_tx_build(frame, planar, n->channels, REAC_SAMPLES_PER_PKT, 0, n->src);
			reac_pacer_submit(&n->pacer, frame, REAC_FRAME_BYTES);
			n->staged = 0;
		}
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg)
{
	(void)tx_ring; /* the pacer owns its own frame ring (reac_pacer.ring) */

	struct reac_sink_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->channels = cfg->channels > REAC_MAX_CHANNELS ? REAC_MAX_CHANNELS : cfg->channels;
	n->sample_rate = cfg->sample_rate;

	/* Our master src MAC (Roland OUI stand-in unless the caller supplies one). */
	static const uint8_t standin[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
	memcpy(n->src, cfg->src_mac ? cfg->src_mac : standin, 6);

	/* The pacer is the master + the cadence clock. fps = rate / 12 (downstream is
	 * 12 samples/frame at every rate). It opens the AF_PACKET TX socket. */
	struct reac_pacer_cfg pcfg = {
		.ifname = cfg->ifname,
		.fps = n->sample_rate / REAC_SAMPLES_PER_PKT,
		.prio = 0,        /* default 79 */
		.cpu = -1,        /* no pin by default (set on a dedicated rig host) */
		.src_mac = n->src,
	};
	if (reac_pacer_open(&n->pacer, &pcfg) != 0) {
		pw_log_warn("reac:playback — cannot open AF_PACKET TX on '%s' "
		            "(needs CAP_NET_RAW + a valid interface); sink not created",
		            cfg->ifname);
		free(n);
		return NULL;
	}
	n->pacer_open = 1;

	char rate_str[16];
	snprintf(rate_str, sizeof rate_str, "1/%d", n->sample_rate);

	n->filter = pw_filter_new_simple(
		loop,
		"reac:playback",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback", /* a sink consumes audio */
			PW_KEY_MEDIA_CLASS, "Audio/Sink",  /* shows up as an output device */
			PW_KEY_NODE_NAME, "reac-playback",
			PW_KEY_NODE_DESCRIPTION, "REAC 40ch playback (downstream master TX)",
			/* The wire is the rate authority; advertise the REAC rate so PipeWire
			 * resamples whatever the app plays into our pps. */
			PW_KEY_NODE_RATE, rate_str,
			NULL),
		&filter_events, n);
	if (!n->filter) {
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}

	for (int c = 0; c < n->channels; c++) {
		char pname[24];
		snprintf(pname, sizeof pname, "playback_%02d", c + 1);
		n->ports[c] = pw_filter_add_port(
			n->filter,
			PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port_in),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, pname,
				NULL),
			NULL, 0);
		if (n->ports[c])
			n->ports[c]->channel = c;
	}

	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		pw_filter_destroy(n->filter);
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}

	/* Start the SCHED_FIFO pacer thread. The master FSM probes immediately and
	 * unconditionally (a real unlinked M-5000 always hunts) and only GRANTS on
	 * the box's own cold-connect — no presence assumption, no timer advance
	 * (defect #130). Audio FILLER flows in every state, so the loopback demo
	 * still hears the stream while the FSM stays honestly in PROBING. */
	if (reac_pacer_start(&n->pacer) != 0) {
		pw_log_warn("reac:playback — cannot start cadence pacer thread");
		pw_filter_destroy(n->filter);
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}

	pw_log_info("reac:playback MASTER on '%s' (%d ch, %d Hz, %d fps pacer) — "
	            "probing; establishment is event-driven on the box's JOIN",
	            cfg->ifname, n->channels, n->sample_rate,
	            n->sample_rate / REAC_SAMPLES_PER_PKT);
	return n;
}

void reac_sink_node_destroy(struct reac_sink_node *n)
{
	if (!n)
		return;
	if (n->filter)
		pw_filter_destroy(n->filter);   /* stops process() submits first */
	if (n->pacer_open) {
		reac_pacer_stop(&n->pacer);     /* join the RT thread */
		reac_pacer_close(&n->pacer);    /* close socket + free ring */
	}
	free(n);
}
