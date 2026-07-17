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
#include "reac_gain.h"
#include "reac_headamp_prop.h"   /* live head-amp control parse (task #203) */
#include "reac_link_state.h"
#include "reac_lat.h"        /* ProcessLatency smoothing (task #152) */
#include "reac_ctrl.h"       /* struct reac_box_model (recognized-box props) */
#include "reac_mac.h"

#include <reac/reac.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

/* Volume ramp length: how long a level change (or mute/unmute) glides so it does
 * not zipper. ~15 ms is the usual de-click window — long enough to be inaudible
 * as a step, short enough that an operator move feels immediate. Converted to a
 * per-sample linear increment from the wire rate at construction. */
#define REAC_GAIN_RAMP_MS   15.0f

/* Advertised upper bound of the volume range (linear). 10.0 == +20 dB of
 * over-amplification headroom, matching the range PipeWire sinks commonly expose;
 * the encoder's f32->s24 clamp still bounds the actual wire sample, so a boosted
 * gain saturates cleanly rather than wrapping. */
#define REAC_GAIN_VOL_MAX   10.0f

/* Graph->wire delay (task #152). reac:playback is the audio endpoint (the wire),
 * so the delay a sample sees is the 12-sample staging accumulator plus the pacer
 * frame-ring depth — each queued frame is REAC_SAMPLES_PER_PKT samples clocked
 * out one per slot. We advertise it as SPA_PARAM_ProcessLatency so PipeWire's
 * latency algorithm accounts for it (A/V sync, `pw-top`), which the node did not
 * do before (it published an empty ProcessLatency). The ring depth SAWTOOTHS with
 * each producer burst (measured live ~38..113 frames), so the value is SMOOTHED
 * (reac_lat: an EMA of the drain-observed depth, converging to the sawtooth mean)
 * and re-advertised only on a material sustained step — see reac_lat.h. */

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

	/* Output-gain state (SPA_PROP volume/mute; see reac_gain.h for the scale).
	 * Split by thread ownership, no locks:
	 *   - chan_vol[] + muted: MAIN-LOOP shadow, written only by param_changed;
	 *     the linear per-channel volume + the mute flag as the controller set
	 *     them. Also used to re-advertise the current Props.
	 *   - chan_target[]: the effective per-channel gain (mute folded in) the RT
	 *     thread ramps toward. Written by param_changed (main loop), read by the
	 *     RT process() — a single relaxed atomic per channel, the SPSC pattern
	 *     the source node uses for its telemetry.
	 *   - chan_cur[]: RT-ONLY running ramp gain; only process() touches it.
	 *   - ramp_step: per-sample linear increment, const after construction. */
	float chan_vol[REAC_MAX_CHANNELS];
	bool muted;
	_Atomic float chan_target[REAC_MAX_CHANNELS];
	float chan_cur[REAC_MAX_CHANNELS];
	float ramp_step;

	/* reac.link-state / reac.box-model / reac.box-width (task #154's stagebox-
	 * badge need): MAIN-LOOP-only shadow of what was last stamped into the
	 * filter's node properties, so on_log_timer only calls
	 * pw_filter_update_properties when something actually changed. Sourced from
	 * the pacer's cross-thread-safe atomics (fsm_state, drops[], recognized_box)
	 * — never touched from on_process (RT). */
	enum reac_link_state link_state_last;
	uint64_t link_drops_seen;               /* sum of pacer.drops[] last poll */
	const struct reac_box_model *box_model_last;

	/* reac.discovery.* (task #178): MAIN-LOOP-only shadow of the seq last stamped into
	 * the filter's node properties, so on_log_timer re-publishes only when the discovery
	 * table actually changed — the reac.link-state pattern above, keyed on seq.
	 * Initialised to 0 to match the "0"/"[]" seeded at node creation: seq 0 is a REAL,
	 * published state ("listening, nothing seen yet"), which a reader must be able to
	 * tell apart from reac-pw publishing no discovery keys at all. */
	uint32_t disco_seq_last;
	const char *disco_ifname;               /* the segment we can honestly speak for */

	/* ProcessLatency smoother (task #152): EMA of the drain-observed ring depth +
	 * re-advertise hysteresis, driven from the 200 ms log timer. See reac_lat.h. */
	struct reac_lat lat;
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

	/* Publish the graph quantum so the pacer's depth guard sizes its band off the
	 * ACTUAL producer burst (up to one quantum of frames pushed per callback), not
	 * a guessed steady state. A single relaxed atomic store; RT-safe. */
	atomic_store_explicit(&n->pacer.graph_quantum, nframes, memory_order_relaxed);

	const float *in[REAC_MAX_CHANNELS];
	int have = 0;
	for (int c = 0; c < n->channels; c++) {
		/* Belt-and-braces: a NULL port (should never happen — the constructor now
		 * fails if add_port returns NULL) is treated as unlinked, so we never deref
		 * NULL in the RT path; the stage just carries silence for that slot. */
		float *b = n->ports[c] ? pw_filter_get_dsp_buffer(n->ports[c], nframes) : NULL;
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
			/* Apply the SPA_PROP output gain to each channel's full 12-sample
			 * stage before encoding — a per-sample linear ramp toward the
			 * controller's latest target so a volume/mute move glides instead of
			 * clicking. reac_gain_ramp_block is pure (no atomic/alloc/syscall);
			 * we only load the target atomic (relaxed) and carry chan_cur. This
			 * is the defense-in-depth so wpctl/desktop volume attenuates the box
			 * DAC even though the raw filter has no audioadapter. */
			for (int c = 0; c < n->channels; c++) {
				float target = atomic_load_explicit(&n->chan_target[c],
				                                     memory_order_relaxed);
				n->chan_cur[c] = reac_gain_ramp_block(n->stage[c],
				                                      REAC_SAMPLES_PER_PKT,
				                                      n->chan_cur[c], target,
				                                      n->ramp_step);
			}
			/* Encode audio + L2 header; counter/control are stamped by the pacer.
			 * Counter 0 is a placeholder (overwritten on egress). */
			reac_tx_build(frame, planar, n->channels, REAC_SAMPLES_PER_PKT, 0, n->src);
			reac_pacer_submit(&n->pacer, frame, REAC_FRAME_BYTES);
			n->staged = 0;
		}
	}
}

/* Build the node's param pods into `b`: the three PropInfo descriptors (volume,
 * mute, channelVolumes) that let a controller discover the controls, plus a Props
 * object carrying the CURRENT state (volume/mute/channelVolumes/channelMap). Used
 * both to advertise at connect and to re-advertise after a change so controllers
 * (and WirePlumber's state store) always see the live values. Returns the count.
 *
 * MAIN LOOP only: reads chan_vol[]/muted, which only param_changed writes. */
static uint32_t sink_build_params(struct reac_sink_node *n, struct spa_pod_builder *b,
                                  const struct spa_pod *params[5])
{
	params[0] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
		SPA_PROP_INFO_id,          SPA_POD_Id(SPA_PROP_volume),
		SPA_PROP_INFO_description, SPA_POD_String("Volume"),
		SPA_PROP_INFO_type,        SPA_POD_CHOICE_RANGE_Float(1.0f, 0.0f, REAC_GAIN_VOL_MAX));

	params[1] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
		SPA_PROP_INFO_id,          SPA_POD_Id(SPA_PROP_mute),
		SPA_PROP_INFO_description, SPA_POD_String("Mute"),
		SPA_PROP_INFO_type,        SPA_POD_CHOICE_Bool(false));

	params[2] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
		SPA_PROP_INFO_id,          SPA_POD_Id(SPA_PROP_channelVolumes),
		SPA_PROP_INFO_description, SPA_POD_String("Channel Volumes"),
		SPA_PROP_INFO_type,        SPA_POD_CHOICE_RANGE_Float(1.0f, 0.0f, REAC_GAIN_VOL_MAX),
		SPA_PROP_INFO_container,   SPA_POD_Id(SPA_TYPE_Array));

	/* Head-amp control (task #203). Discoverable so a controller sees that this
	 * master node accepts per-channel preamp commands; the values ride SPA_PROP_params
	 * (an extensible (key,value) list) as "reac.headamp.<ch>.{phantom,pad,sens}" =
	 * absolute setting. Advertised alongside volume/mute; the SET path is
	 * on_param_changed -> reac_headamp_prop_parse -> the pacer command ring. Unlike
	 * volume, head-amp state is NOT echoed in the Props state object below — it is
	 * write-through control re-asserted on the wire by the DMX scheduler, not a node
	 * property to read back. */
	params[3] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
		SPA_PROP_INFO_id,          SPA_POD_Id(SPA_PROP_params),
		SPA_PROP_INFO_description, SPA_POD_String(
			"REAC head-amp: params \"reac.headamp.<ch>.{phantom,pad,sens}\" = value"),
		SPA_PROP_INFO_type,        SPA_POD_String("reac.headamp.<ch>.<param>"));

	/* Current state. The channelMap mirrors the AUX ports (playback_NN -> AUXc),
	 * so a controller's per-channel sliders line up with the box outputs; `volume`
	 * is the mono view (mean of the per-channel gains). */
	float vols[REAC_MAX_CHANNELS];
	uint32_t map[REAC_MAX_CHANNELS];
	float sum = 0.0f;
	for (int c = 0; c < n->channels; c++) {
		vols[c] = n->chan_vol[c];
		map[c]  = SPA_AUDIO_CHANNEL_AUX0 + c;
		sum    += n->chan_vol[c];
	}
	float vmean = n->channels > 0 ? sum / (float)n->channels : 1.0f;

	params[4] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
		SPA_PROP_volume,         SPA_POD_Float(vmean),
		SPA_PROP_mute,           SPA_POD_Bool(n->muted),
		SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
		                                       n->channels, vols),
		SPA_PROP_channelMap,     SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id,
		                                       n->channels, map));
	return 5;
}

/* Push the current per-channel gain (mute folded in) to the RT thread and
 * re-advertise the Props so controllers see the live state. MAIN LOOP only. */
static void sink_publish(struct reac_sink_node *n)
{
	for (int c = 0; c < n->channels; c++) {
		float t = n->muted ? 0.0f : n->chan_vol[c];
		atomic_store_explicit(&n->chan_target[c], t, memory_order_relaxed);
	}
	if (!n->filter)
		return;
	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[5];
	uint32_t np = sink_build_params(n, &b, params);
	pw_filter_update_params(n->filter, NULL, params, np);
}

/* MAIN LOOP: a controller changed our node params. We only care about node-global
 * Props (port_data == NULL). Parse volume / mute / channelVolumes and re-publish.
 * channelVolumes is authoritative per-channel; a bare `volume` scalar sets all
 * channels (so both a mono and a per-channel controller work, with no double
 * count). Values are linear (reac_gain.h); negatives clamp to silence. */
static void on_param_changed(void *data, void *port_data, uint32_t id,
                             const struct spa_pod *param)
{
	struct reac_sink_node *n = data;
	if (port_data != NULL)             /* a port param, not the node's Props */
		return;
	if (id != SPA_PARAM_Props || param == NULL)
		return;

	const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
	const struct spa_pod_prop *prop;
	float chanvols[REAC_MAX_CHANNELS];
	uint32_t nchv = 0;
	bool have_chanvol = false, changed = false;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute: {
			bool m;
			if (spa_pod_get_bool(&prop->value, &m) == 0) {
				n->muted = m;
				changed = true;
			}
			break;
		}
		case SPA_PROP_volume: {
			float v;
			if (spa_pod_get_float(&prop->value, &v) == 0) {
				if (v < 0.0f)
					v = 0.0f;
				for (int c = 0; c < n->channels; c++)
					n->chan_vol[c] = v;
				changed = true;
			}
			break;
		}
		case SPA_PROP_channelVolumes:
			nchv = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
			                          chanvols, REAC_MAX_CHANNELS);
			if (nchv > 0)
				have_chanvol = true;
			break;
		default:
			break;
		}
	}

	/* channelVolumes wins over a same-object `volume` scalar (applied last). */
	if (have_chanvol) {
		for (uint32_t c = 0; c < nchv && c < (uint32_t)n->channels; c++)
			n->chan_vol[c] = chanvols[c] < 0.0f ? 0.0f : chanvols[c];
		changed = true;
	}

	/* LIVE head-amp control (task #203): the SAME Props object may carry per-channel
	 * phantom/pad/sens changes under SPA_PROP_params ("reac.headamp.<ch>.<param>").
	 * Parse them (pure, no state touched here) and hand each to the pacer's lock-free
	 * command ring — the RT pacer thread applies them to the head-amp DMX send table,
	 * so a mixer knob reaches the real box preamp live. This is the master node (the
	 * sink owns the pacer), so it is master-role by construction; a slave has no
	 * pacer/head-amp send path. Independent of the volume/mute `changed` re-publish
	 * above — head-amp state is not echoed back in Props (it is write-through control,
	 * re-asserted on the wire by the DMX scheduler, not a node property). */
	struct reac_headamp_setting ha[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	int nha = reac_headamp_prop_parse(param, ha,
	                                  (int)(sizeof ha / sizeof ha[0]));
	for (int i = 0; i < nha; i++)
		reac_pacer_headamp_set(&n->pacer, ha[i].ch, ha[i].param, ha[i].value);

	if (changed)
		sink_publish(n);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
	.param_changed = on_param_changed,
};

/* MAIN LOOP: stamp reac.link-state / reac.box-model / reac.box-width (task
 * #154's stagebox-badge need) from the pacer's cross-thread-safe snapshot,
 * re-advertising via pw_filter_update_properties only when something actually
 * changed since the last poll — the node-properties analogue of sink_publish's
 * SPA_PARAM_Props re-advertise. Reads ONLY atomics the pacer thread already
 * publishes for cross-thread use (fsm_state, drops[], recognized_box); never
 * touches the RT process() path. Called from on_log_timer, the pacer's
 * existing non-RT drain hook — see reac_link_state.h for the mapping + the
 * "dropped" one-shot-overlay rationale. */
static void sink_publish_link_props(struct reac_sink_node *n)
{
	if (!n->filter)
		return;

	uint64_t drops_total = 0;
	for (int i = 0; i < 8; i++)
		drops_total += atomic_load_explicit(&n->pacer.drops[i], memory_order_relaxed);
	int just_dropped = (drops_total != n->link_drops_seen);
	n->link_drops_seen = drops_total;

	enum reac_master_state st = (enum reac_master_state)
		atomic_load_explicit(&n->pacer.fsm_state, memory_order_acquire);
	enum reac_link_state ls = reac_link_state_from_master(st, just_dropped);

	const struct reac_box_model *bm =
		atomic_load_explicit(&n->pacer.recognized_box, memory_order_acquire);

	if (ls == n->link_state_last && bm == n->box_model_last)
		return; /* unchanged: do not spam pw_filter_update_properties */
	n->link_state_last = ls;
	n->box_model_last = bm;

	char width[16];
	if (bm)
		snprintf(width, sizeof width, "%dx%d", bm->in_ch, bm->out_ch);
	else
		snprintf(width, sizeof width, "0x0");

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_LINK_STATE, reac_link_state_name(ls),
		REAC_PROP_BOX_MODEL,  bm ? bm->token : "none",
		REAC_PROP_BOX_WIDTH,  width,
		NULL);
	if (props) {
		pw_filter_update_properties(n->filter, NULL, &props->dict);
		pw_properties_free(props);
	}
}

/* MAIN LOOP: stamp reac.discovery.* — WHAT IS ON THIS SEGMENT, as opposed to what this
 * master joined (task #178). The engine cannot do this itself: openmixer runs as a
 * `systemctl --user` unit whose node has no CAP_NET_RAW (measured: CapEff 0), a user
 * manager cannot grant a capability it does not hold, and setcap on the shared `node`
 * binary would arm every Node process on the box. reac-pw already holds the capability
 * and already decodes the frames, so it publishes what it sees and the engine reads it
 * off the registry — the same seam reac.link-state already travels on.
 *
 * Publishes four keys, and the honesty of the set rests on all four:
 *   scope   — the ONE interface these results speak for. Silence about a NIC we never
 *             watched must never read as "nothing is there".
 *   state   — "listening": passive only. reac-pw's discovery TRANSMITS NOTHING; it reads
 *             frames the promiscuous socket already receives. That is what makes it safe
 *             on the operator's live segment, where active-probing could disturb a
 *             joined box.
 *   seq     — bumped on every real change. A frozen seq lets a reader detect a wedged
 *             publisher instead of trusting a stale list.
 *   devices — the JSON snapshot, all-or-nothing.
 *
 * Reads p->disco, which is MAIN-THREAD-ONLY (built by reac_pacer_log_drain, called from
 * this same timer just above) — no atomics needed and none used. */
static void sink_publish_disco_props(struct reac_sink_node *n)
{
	if (!n->filter)
		return;
	if (n->pacer.disco.seq == n->disco_seq_last)
		return;   /* unchanged: do not spam pw_filter_update_properties */

	char devices[REAC_DISCO_JSON_MAX];
	if (reac_disco_table_json(&n->pacer.disco, reac_pacer_mono_ns(), devices, sizeof devices) < 0)
		return;   /* would not fit: publish NOTHING rather than a truncated list that
		           * still parses — as a shorter, wrong set of devices. Leave the last
		           * good snapshot standing and retry on the next change. */

	char seq[16];
	snprintf(seq, sizeof seq, "%u", n->pacer.disco.seq);

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_DISCO_SCOPE,   n->disco_ifname ? n->disco_ifname : "",
		REAC_PROP_DISCO_STATE,   REAC_DISCO_STATE_LISTENING,
		REAC_PROP_DISCO_SEQ,     seq,
		REAC_PROP_DISCO_DEVICES, devices,
		NULL);
	if (props) {
		pw_filter_update_properties(n->filter, NULL, &props->dict);
		pw_properties_free(props);
		n->disco_seq_last = n->pacer.disco.seq;
	}
}

/* MAIN LOOP: advertise the node's graph->wire delay as SPA_PARAM_ProcessLatency
 * so PipeWire's latency algorithm folds it into the graph latency (the default
 * pw_filter latency handling applies a node ProcessLatency — we do NOT set
 * PW_FILTER_FLAG_CUSTOM_LATENCY). The instantaneous ring depth sawtooths with the
 * producer bursts, so reporting it raw churns the graph every poll; instead feed
 * the drain-observed depth through the reac_lat EMA smoother, which yields a
 * STABLE contract latency and re-advertises only on a material sustained step
 * (see reac_lat.h). reac_frame_ring_readable is a 2-atomic load, safe on the
 * non-RT path. Returns immediately (no update_params) when the value is stable. */
static void sink_publish_latency(struct reac_sink_node *n)
{
	if (!n->filter)
		return;

	uint32_t depth = n->pacer_open ? reac_frame_ring_readable(&n->pacer.ring) : 0;
	int64_t lat_ns;
	if (!reac_lat_poll(&n->lat, depth, n->sample_rate, &lat_ns))
		return;   /* smoothed latency unchanged: no re-advertise, no churn */

	uint32_t lat_samples = n->sample_rate > 0
		? (uint32_t)((double)lat_ns * (double)n->sample_rate / 1e9 + 0.5)
		: 0;
	struct spa_process_latency_info pl = {
		.quantum = 0.0f,
		.rate = (int32_t)lat_samples,   /* samples at the node (wire) rate */
		.ns = lat_ns,
	};
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *param =
		spa_process_latency_build(&b, SPA_PARAM_ProcessLatency, &pl);
	pw_filter_update_params(n->filter, NULL, &param, 1);
}

/* MAIN LOOP (non-RT): drain the pacer's FSM event ring to stderr, then
 * re-stamp the link-state node properties from the same non-RT cadence. The
 * pacer thread is SCHED_FIFO and must not touch stdio (or PipeWire API); it
 * logs into a lock-free ring and this 200 ms timer formats it — so a live
 * power-cycle prints the complete establishment transcript (presence edges,
 * JOIN hex dumps, transitions) AND keeps reac.link-state live. */
static void on_log_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct reac_sink_node *n = data;
	reac_pacer_log_drain(&n->pacer, stderr);
	sink_publish_link_props(n);
	sink_publish_disco_props(n);   /* strictly AFTER the drain: it builds pacer.disco */
	sink_publish_latency(n);
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

	/* Output gain starts at UNITY (calloc would leave it 0 == fully muted). The
	 * ramp step is one REAC_GAIN_RAMP_MS worth of samples at the wire rate; a
	 * safe fallback keeps it positive if the rate is somehow unset. */
	float ramp_samples = REAC_GAIN_RAMP_MS * (float)n->sample_rate / 1000.0f;
	n->ramp_step = ramp_samples > 1.0f ? 1.0f / ramp_samples : 1.0f;
	n->muted = false;
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
		n->chan_vol[c] = 1.0f;
		n->chan_cur[c] = 1.0f;
		atomic_init(&n->chan_target[c], 1.0f);
	}

	/* Our master src MAC. The caller (main.c master path) supplies the impersonated
	 * desk's MAC; absent that, derive the Roland-OUI + this-NIC's-host-part default
	 * from the one helper (reac_mac.h) rather than a scattered hard-coded host part. */
	if (cfg->src_mac)
		memcpy(n->src, cfg->src_mac, 6);
	else
		reac_mac_default_src(cfg->ifname, n->src);

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
	pcfg.headamps = cfg->headamps;        /* master head-amp DMX table (may be NULL) */
	pcfg.n_headamps = cfg->n_headamps;
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
			/* Correct-at-boot badge props (task #154): the pacer thread hasn't
			 * started yet at this point, but a real master enters PROBING
			 * unconditionally on its first frame (#130) — IDLE is a sub-ms
			 * transient, so "probing" is truthful from t=0. Kept live by
			 * sink_publish_link_props on the 200 ms log-timer below. */
			REAC_PROP_LINK_STATE, reac_link_state_name(REAC_LINK_PROBING),
			REAC_PROP_BOX_MODEL, "none",
			REAC_PROP_BOX_WIDTH, "0x0",
			/* Correct-at-boot discovery (task #178): from t=0 we are listening on
			 * this NIC and have seen nothing yet — which is the truth, and is NOT
			 * the same claim as "there is nothing here". Publishing the keys
			 * immediately is what lets a reader tell a listening-but-empty reac-pw
			 * apart from one that predates discovery (keys absent = could not scan).
			 * Kept live by sink_publish_disco_props on the 200 ms log-timer. */
			REAC_PROP_DISCO_SCOPE, cfg->ifname ? cfg->ifname : "",
			REAC_PROP_DISCO_STATE, REAC_DISCO_STATE_LISTENING,
			REAC_PROP_DISCO_SEQ, "0",
			REAC_PROP_DISCO_DEVICES, "[]",
			NULL),
		&filter_events, n);
	if (!n->filter) {
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}
	n->link_state_last = REAC_LINK_PROBING;
	/* Matches the "0"/"[]" seeded above, so an empty segment never triggers a
	 * redundant re-publish; a first real sighting bumps seq to 1 and does. */
	n->disco_seq_last = 0;
	n->disco_ifname = cfg->ifname;
	n->box_model_last = NULL;
	n->link_drops_seen = 0;
	reac_lat_init(&n->lat);

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
		/* A NULL port would later be handed to pw_filter_get_dsp_buffer on the RT
		 * thread (SPA_CONTAINER_OF on NULL = a wild deref). Fail construction. */
		if (!n->ports[c]) {
			pw_filter_destroy(n->filter);
			reac_pacer_close(&n->pacer);
			free(n);
			return NULL;
		}
		n->ports[c]->channel = c;
	}

	/* Advertise the volume/mute PropInfo + the initial (unity) Props at connect,
	 * so a controller sees the controls the moment the node appears and standard
	 * volume tools drive the box outputs (the raw filter has no audioadapter, so
	 * without this wpctl/desktop volume would be silently ignored). */
	uint8_t pbuf[2048];
	struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pbuf, sizeof pbuf);
	const struct spa_pod *cparams[5];
	uint32_t ncp = sink_build_params(n, &pb, cparams);

	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_RT_PROCESS, cparams, ncp) < 0) {
		pw_filter_destroy(n->filter);
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}

	/* Advertise the graph->wire delay now (the first poll seeds the EMA from the
	 * still-empty ring and always advertises). Kept live + smoothed by
	 * sink_publish_latency on the 200 ms log timer below as the ring depth moves. */
	sink_publish_latency(n);

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
