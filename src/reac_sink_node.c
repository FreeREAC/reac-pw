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
	struct pw_loop *loop;
	struct spa_source *log_timer;  /* 200 ms event-log drain on the main loop */
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

	/* MUST be zeroed: reac_tx_build only encodes n->channels of the 40 downstream
	 * slots, so the unused slots would otherwise carry uninitialized stack memory
	 * onto the wire. A real M-200 sends CLEAN ZEROS in every FILLER's audio region
	 * while hunting (measured: 100% zero vs our 100% non-zero) — #130. */
	uint8_t frame[REAC_FRAME_BYTES] = { 0 };
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

/* MAIN LOOP (non-RT): drain the pacer's FSM event ring to stderr. The pacer
 * thread is SCHED_FIFO and must not touch stdio; it logs into a lock-free ring
 * and this 200 ms timer formats it — so a live power-cycle prints the complete
 * establishment transcript (presence edges, JOIN hex dumps, transitions). */
static void on_log_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct reac_sink_node *n = data;
	reac_pacer_log_drain(&n->pacer, stderr);
}

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
		/* Advertise the S-1608 downstream (8 out / 16 in); the console_field is
		 * the emulated mixer model, from the --mixer profile (default V-Mixer). */
		.console = REAC_CONSOLE_CFG_S1608,
	};
	pcfg.console.console_field = cfg->console_field;
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

	char nodename[64];
	if (cfg->inst && *cfg->inst)
		snprintf(nodename, sizeof nodename, "reac-playback.%s", cfg->inst);
	else
		snprintf(nodename, sizeof nodename, "reac-playback");
	char desc[128];
	if (cfg->label && *cfg->label)
		snprintf(desc, sizeof desc, "%s — %d ch (REAC box outputs)", cfg->label, n->channels);
	else
		snprintf(desc, sizeof desc, "REAC %dch playback (downstream master TX)", n->channels);

	n->filter = pw_filter_new_simple(
		loop,
		"reac:playback",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback", /* a sink consumes audio */
			PW_KEY_MEDIA_CLASS, "Audio/Sink",  /* shows up as an output device */
			PW_KEY_NODE_NAME, nodename,
			PW_KEY_NODE_DESCRIPTION, desc,
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
		char pname[24], achan[12];
		snprintf(pname, sizeof pname, "playback_%02d", c + 1);
		/* Discrete mono box output — AUX channel so no tool pairs them as stereo. */
		snprintf(achan, sizeof achan, "AUX%d", c);
		n->ports[c] = pw_filter_add_port(
			n->filter,
			PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port_in),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, pname,
				PW_KEY_AUDIO_CHANNEL, achan,
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

	/* The FSM/RX log drain: 200 ms period on the main loop we already hold. */
	n->loop = loop;
	n->log_timer = pw_loop_add_timer(loop, on_log_timer, n);
	if (n->log_timer) {
		struct timespec first = { 0, 200 * 1000000L };
		struct timespec interval = { 0, 200 * 1000000L };
		pw_loop_update_timer(loop, n->log_timer, &first, &interval, false);
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
	if (n->log_timer)
		pw_loop_destroy_source(n->loop, n->log_timer);
	if (n->filter)
		pw_filter_destroy(n->filter);   /* stops process() submits first */
	if (n->pacer_open) {
		reac_pacer_stop(&n->pacer);     /* join the RT thread */
		/* Final drain + counters: the shutdown summary of the establishment. */
		reac_pacer_log_drain(&n->pacer, stderr);
		fprintf(stderr,
		        "reac-master: shutdown in %s — tx=%llu err=%llu late=%llu | "
		        "rx_box_frames=%llu rx_box_ctrl=%llu rx_joins=%llu "
		        "grant_attempts=%llu | drops: peer-gone=%llu bye=%llu "
		        "mac-change=%llu grant-timeout=%llu | log-drops=%llu\n",
		        reac_master_state_name(n->pacer.master.state),
		        (unsigned long long)n->pacer.tx_frames,
		        (unsigned long long)n->pacer.tx_errors,
		        (unsigned long long)n->pacer.late_wakes,
		        (unsigned long long)n->pacer.rx_box_frames,
		        (unsigned long long)n->pacer.rx_box_ctrl,
		        (unsigned long long)n->pacer.rx_joins,
		        (unsigned long long)n->pacer.grant_attempts,
		        (unsigned long long)n->pacer.drops[REAC_M_DROP_PEER_GONE],
		        (unsigned long long)n->pacer.drops[REAC_M_DROP_BYE],
		        (unsigned long long)n->pacer.drops[REAC_M_DROP_MAC_CHANGE],
		        (unsigned long long)n->pacer.drops[REAC_M_DROP_GRANT_TIMEOUT],
		        (unsigned long long)n->pacer.ev_drops);
		reac_pacer_close(&n->pacer);    /* close socket + free ring */
	}
	free(n);
}
