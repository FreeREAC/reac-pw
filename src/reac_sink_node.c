// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac:playback — Audio/Sink that encodes the graph's PCM into REAC downstream
 * frames and hands them to the SCHED_FIFO cadence pacer (reac_pacer), which is
 * the REAC MASTER:
 *
 *   - registers an Audio/Sink with `channels` mono DSP input ports, so apps
 *     (Rhythmbox, pw-play, ...) and the graph can play INTO it;
 *   - process() de-stages each quantum into 12-sample REAC frames, encodes them
 *     with libreac's reac_downstream_build, and SUBMITS them to the pacer's TX ring (a lock-free
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
#include "reac_source_node.h" /* peer reac-capture badge push (#208) */
#include "reac_tx.h"
#include "reac_pacer.h"
#include "reac_gain.h"
#include <spa/node/io.h>   /* struct spa_io_rate_match + SPA_IO_RateMatch */
#include "reac_headamp_prop.h"   /* live head-amp control parse (task #203) */
#include "reac_rate_cfg.h"       /* live reac.cfg.rate parse + decision core */
#include "reac_sink_format.h"    /* the Format pod + renegotiate decision (#4.3) */
#include "reac_role.h"           /* enum reac_role — this node is MASTER-only */
#include "reac_role_cfg.h"       /* live reac.cfg.role parse + decision core */
#include "reac_link_state.h"
#include "reac_arbitration.h"
#include "reac_lat.h"        /* ProcessLatency smoothing (task #152) */
#include "reac_ctrl.h"       /* struct reac_box_model (recognized-box props) */
#include "reac_mac.h"
#include "reac_rx.h"      /* the BOX clock reference measurement source (#75) */

#include <reac/reac.h>
#include <reac/reac_encode.h>  /* reac_downstream_build — libreac owns the frame layout */
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
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
	/* AN ADAPTER, NOT A RAW FILTER (2026-08-21). A filter's ports are raw DSP ports
	 * at the GRAPH rate with no audioconvert, so the node could only run at the
	 * graph's pace and we made up the difference by TRIMMING the ring — 224978 frames
	 * discarded in one measured run, audible as granulated, saturated sound. The REAC
	 * pace and the rig pace are independent and conversion belongs here, in the
	 * adapter, so this is a pw_stream declaring the REAC rate in its FORMAT and
	 * PipeWire resamples. See docs/design/specs/2026-08-21-reac-adapter-pace-and-
	 * port-contract.md in openmixer. */
	struct pw_stream *stream;
	struct reac_pacer pacer;
	int pacer_open;
	int channels;             /* current filter port count; 0 = no filter yet */
	int sample_rate;
	uint8_t src[6];           /* our master MAC */
	struct pw_loop *loop;
	const char *inst;         /* per-instance node suffix (for filter (re)build) */
	char label[64];           /* effective box label on the node description ("" = none) */
	char clock_ref[64];       /* operator-designated clock reference (#77); "" = none.
	                           * OWN copy, not the caller's pointer: it is read from
	                           * the RT graph thread on every quantum. Const after
	                           * construction, so no synchronisation is needed. */
	struct spa_source *log_timer;  /* 200 ms event-log drain on the main loop */
	/* SPA_IO_Position, captured via io_changed — the stream's equivalent of the
	 * argument pw_filter handed process(). */
	const struct spa_io_position *position;

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

	/* reac.rate / reac.rate.source / reac.cfg.rate.state / reac.cfg.rate.refused
	 * (2026-08-26-reac-runtime-config.md): same shadow-and-compare pattern as
	 * the link-state trio above, so sink_publish_rate_props only re-stamps the
	 * filter's properties when the pacer's rate atomics actually moved. */
	int rate_hz_last;
	int rate_asserted_last;
	int rate_reestablishing_last;
	enum reac_rate_refuse rate_refused_last;

	/* Set around sink_reconnect_rate's pw_stream_disconnect/connect pair
	 * (increment 4). MAIN-LOOP-only write; on_process (RT, a different
	 * thread under PW_STREAM_FLAG_RT_PROCESS) reads it as the FIRST check,
	 * relaxed load, as a second, cheap line of defense on top of the
	 * load-bearing guarantee that already covers this: PipeWire does not
	 * invoke process() on a disconnected stream (the same guarantee
	 * reac_sink_node_ensure's destroy+rebuild has relied on since the
	 * box-width path landed). See sink_reconnect_rate for why this window
	 * is otherwise harmless even without the flag. */
	_Atomic int rate_reconnecting;

	/* reac.role / reac.cfg.role.state / reac.cfg.role.refused
	 * (2026-08-26-reac-runtime-config.md, the ROLE half): MAIN-LOOP-only,
	 * unlike the rate trio above this needs no cross-thread atomic — this
	 * increment's role apply never touches the pacer/RT thread at all (see
	 * reac_role_cfg.h's HONESTY note), so on_param_changed (which decides the
	 * answer) and sink_publish_role_props (which stamps it) are both
	 * main-loop-only and can share plain fields, the same way chan_vol/muted
	 * already do. role_state/role_refused are the CURRENT answer;
	 * *_last are the shadow sink_publish_role_props compares against, seeded
	 * to values the real answer can never equal so the very first publish
	 * always fires. */
	const char *role_state;
	enum reac_role_refuse role_refused;
	const char *role_state_last;
	enum reac_role_refuse role_refused_last;

	/* #208: the peer reac-capture node's SLOT (main's `&src`), so the same log-timer
	 * that keeps THIS sink's badge live also drives the source's — that node has no
	 * pacer handle of its own. A SLOT (not the node) so a source rebuilt on a live
	 * box-width change is followed automatically. NULL when no peer was wired. */
	struct reac_source_node **peer_src;

	/* The RX feeder, borrowed from main, as the BOX clock reference's measurement
	 * source (#75). NULL when the caller wired none — the box tier is then simply
	 * never available. box_ppm_seq is the last estimate we forwarded, so a stalled
	 * feeder stops publishing instead of re-vouching for its last number. */
	struct reac_rx *rate_src;
	uint32_t box_ppm_seq;

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

	/* TX RATE MATCHING (workstream CLK). The area PipeWire's adapter resampler
	 * reads; NULL until the graph gives us one (and forever if the link needs no
	 * resampler). Written on the RT process() thread, read there too. */
	struct spa_io_rate_match *rate_match;
	int rate_match_off;              /* const after open; REACPW_RATE_MATCH=0 */
	/* The correction currently applied, in milli-ppm. Written by the RT thread,
	 * read by the 200 ms property poll — one relaxed atomic each way. */
	_Atomic int rate_match_milli_ppm;
	/* Have we ever been handed a rate-match area? Distinguishes "no resampler on
	 * this link" from "a resampler we are steering to zero". */
	_Atomic int rate_match_present;

	/* Health window snapshot, refreshed by the 200 ms poll and published as node
	 * properties when a window closes. */
	struct reac_pacer_health health;
};

/* REALTIME. Pull this quantum's PCM from the input ports, accumulate into the
 * 12-sample stage, encode a REAC frame for every full group, and SUBMIT it to the
 * pacer's TX ring. No syscall here: the lock-free push hands the frame to the
 * SCHED_FIFO pacer thread, which clocks the wire at a fixed pps. The counter and
 * the master control block are NOT stamped here — the pacer owns the master FSM
 * and stamps them on egress, so the cadence + handshake stay authoritative even
 * across a graph stall. */
static void on_process(void *data)
{
	struct reac_sink_node *n = data;
	if (!n->pacer_open)
		return;
	/* Mid a rate reconnect (sink_reconnect_rate, increment 4): the stream is
	 * disconnected right now, so dequeuing a buffer below would hand back
	 * nothing anyway and this callback should not even be reached — PipeWire
	 * does not drive process() on a disconnected stream. This check is
	 * belt-and-braces documentation of that fact, not the mechanism that
	 * makes it true. Either way: no dequeue, no encode, no submit to the
	 * pacer this cycle — it free-runs FILLER exactly as it already does
	 * whenever nothing is linked (the have == 0 path below). */
	if (atomic_load_explicit(&n->rate_reconnecting, memory_order_relaxed))
		return;
	const struct spa_io_position *position = n->position;
	if (!position)
		return;                 /* no position yet — nothing to pace against */
	struct pw_buffer *pwb = pw_stream_dequeue_buffer(n->stream);
	if (!pwb)
		return;                 /* nothing queued this cycle */
	struct spa_buffer *sbuf = pwb->buffer;
	uint32_t nframes = sbuf->datas[0].chunk->size / (uint32_t)sizeof(float);
	if (nframes > position->clock.duration)
		nframes = position->clock.duration;

	/* Publish the graph quantum so the pacer's depth guard sizes its band off the
	 * ACTUAL producer burst (up to one quantum of frames pushed per callback), not
	 * a guessed steady state. A single relaxed atomic store; RT-safe. */
	atomic_store_explicit(&n->pacer.graph_quantum, nframes, memory_order_relaxed);

	/* ---- TX RATE MATCHING (workstream CLK) ------------------------------------
	 *
	 * ABSORB THE DIFFERENCE INSTEAD OF DISCARDING IT. The producer (this callback,
	 * clocked by the graph) and the consumer (the pacer, clocked by its own
	 * deadline grid) will never agree exactly, and the frame ring is where the
	 * disagreement accumulates. Until now the only thing that ever removed it was
	 * the depth guard, which does so by cutting 256 frames — 64 ms of audio — out
	 * of the ring in one step, silently, with no xrun raised because preventing the
	 * xrun is precisely its job.
	 *
	 * The signal is the ring's own depth against the guard's TARGET, not an
	 * estimate of anyone's oscillator. That is deliberate: a level loop corrects
	 * the error that is actually there, whatever caused it, and it is the same
	 * error the guard was about to correct with a machete.
	 *
	 * PROPORTIONAL, BOUNDED, AND HONEST ABOUT WHAT IT IS. It does not abolish
	 * drift, it moves it into the resampler, so the correction it is applying is
	 * published (reac.health.rate-match-ppm) for an operator to watch. A large and
	 * steady correction is a FAULT REPORT, not a success: it means something
	 * upstream is losing frames and this loop is the only reason it is inaudible.
	 * That is why the pacer's own transmit deficit is fixed at source as well —
	 * see the slot-debt branch in reac_pacer.c. */
	if (n->rate_match && !n->rate_match_off) {
		uint32_t depth  = reac_frame_ring_readable(&n->pacer.ring);
		uint32_t qf     = nframes / (uint32_t)REAC_SAMPLES_PER_PKT;
		/* THE SETPOINT IS NOT THE GUARD'S TARGET, and getting that wrong would have
		 * cost most of what the slot-debt fix just won. The guard's TARGET is 256
		 * frames — 64 ms — because it is a place to drain TO after a pathological
		 * excursion, not a depth the ring should sit at. With the pacer keeping up
		 * (measured: 22-62 frames, 5.5-15.5 ms) a loop that servoed to 256 would
		 * deliberately ADD 50 ms of latency to make room for itself.
		 *
		 * So the setpoint is what the ring actually needs: two producer bursts. The
		 * graph pushes up to one quantum of frames per callback, so two covers a
		 * burst plus the one behind it, and the guard's HIGH stays a ceiling nobody
		 * approaches. On this rig that is 2 x 21 = 42 frames = 10.5 ms — the middle
		 * of where the ring already settles on its own, which is the point: the
		 * matcher's job is to HOLD the ring where the pacer put it, not to fill it. */
		uint32_t target = qf * 2 > 8 ? qf * 2 : 8;
		double err = ((double)depth - (double)target) / (double)target;
		if (err >  1.0) err =  1.0;
		if (err < -1.0) err = -1.0;
		/* depth ABOVE target => we are consuming too slowly => ask the resampler
		 * for fewer samples per second, i.e. a rate below nominal. */
		double ppm = -err * (double)REAC_SINK_RATE_MATCH_MAX_PPM;
		n->rate_match->rate = 1.0 + ppm / 1e6;
		atomic_store_explicit(&n->rate_match_milli_ppm, (int)(ppm * 1000.0),
		                      memory_order_relaxed);
		atomic_store_explicit(&n->rate_match_present, 1, memory_order_relaxed);
	}

	/* GRAPH CLOCK REFERENCE (#75). This callback is the one place that sees the
	 * elected driver's spa_io_clock, and on this rig that driver is the RME the
	 * operator bought for its PLL — the expected configuration, not a fallback.
	 * PipeWire has already done the measurement for us: rate_diff is the driver
	 * clock's speed as a ratio of CLOCK_MONOTONIC, filtered by its own DLL, so we
	 * publish it rather than build a second estimator.
	 *
	 * ADMITTED ONLY WHEN IT IS A REAL, INDEPENDENT CLOCK. A freewheeling graph is
	 * not a clock at all, and a `clock.system.*` driver (PipeWire's dummy timer,
	 * elected when no hardware is in the graph) is timed by CLOCK_MONOTONIC itself
	 * — following it would be following our own free-run through a longer pipe and
	 * would let us report lock while nothing external disciplines anything. Both
	 * are rejected here, which makes the pacer fall back to the box counter slope
	 * or to an honest free-run.
	 *
	 * ADMITTED IS NOT THE SAME AS GOOD ENOUGH (#77). "There is an oscillator
	 * behind this" and "that oscillator is fit to own a REAC segment" are two
	 * questions, and an HDMI sink answers yes to the first and no to the second.
	 * So we also GRADE the device here — this callback is the only place that
	 * holds the driver's name — and publish the grade beside the sample. The
	 * grading itself is deliberately incapable of promoting an unrecognised
	 * device; see reac_clock_name_quality. An operator designation
	 * (REACPW_CLOCK_REF) outranks it, and the DLL's measured stability outranks
	 * them both — that ranking lives in reac_clock_quality_apply, not here.
	 *
	 * Guarded by the knob so the default path costs one predictable branch and not
	 * a single store. RT-safe when it does run: a handful of bounded scans over a
	 * 64-byte name plus four relaxed atomics, no allocation and no syscall. */
	if (n->pacer.clock_follow) {
		const struct spa_io_clock *c = &position->clock;
		int usable = !(c->flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) &&
		             reac_clock_name_is_hardware(c->name);
		enum reac_clock_quality q = reac_clock_grade_name(c->name, n->clock_ref);
		reac_pacer_clock_publish(&n->pacer, REAC_CLOCK_SRC_GRAPH, usable,
		                         (int)(reac_clock_ppm_from_rate_diff(c->rate_diff)
		                               * 1000.0),
		                         c->name, q, c->nsec);
	}

	const float *in[REAC_MAX_CHANNELS];
	int have = 0;
	for (int c = 0; c < n->channels; c++) {
		/* Belt-and-braces: a NULL port (should never happen — the constructor now
		 * fails if add_port returns NULL) is treated as unlinked, so we never deref
		 * NULL in the RT path; the stage just carries silence for that slot. */
		float *b = ((uint32_t)c < sbuf->n_datas) ? sbuf->datas[c].data : NULL;
		in[c] = b;            /* NULL if this port is unlinked this cycle */
		if (b)
			have++;
	}
	if (have == 0) {
		pw_stream_queue_buffer(n->stream, pwb);
		return;               /* nothing feeding us — pacer free-runs silent FILLER */
	}

	float *planar[REAC_MAX_CHANNELS];
	for (int c = 0; c < n->channels; c++)
		planar[c] = n->stage[c];

	/* MUST be zeroed: reac_downstream_build only encodes n->channels of the 40 downstream
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
			reac_downstream_build(frame, planar, n->channels, REAC_SAMPLES_PER_PKT, 0, n->src);
			reac_pacer_submit(&n->pacer, frame, REAC_FRAME_BYTES);
			n->staged = 0;
		}
	}

	/* RETURN THE BUFFER. Every dequeue owes a queue, and this path — the one with
	 * audio actually linked — used to fall out of the function still holding it.
	 * The pool drains within a few quanta, dequeue_buffer then returns NULL
	 * forever, and the early return above turns into the whole steady state: the
	 * sink stops submitting and the pacer free-runs on FILLER.
	 *
	 * Nothing about that looks wrong from outside. Frames keep going out at the
	 * right rate, the pacer's timing still measures clean, the link stays
	 * established, the box stays enrolled — and no audio reaches it. The only
	 * symptom is silence downstream, which is exactly the signal least likely to
	 * be attributed to the sender. It survived because every measurement on this
	 * path reads the box's CAPTURE side, which does not touch this buffer at all. */
	pw_stream_queue_buffer(n->stream, pwb);
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

	/* SPA_PROP_params is the one extensible (key,value) bag both the head-amp
	 * control (task #203) and the runtime rate control
	 * (2026-08-26-reac-runtime-config.md) ride — one PropInfo per underlying
	 * SPA prop id, so both keys are named in this single entry rather than
	 * two competing PropInfo objects with the same id. Advertised alongside
	 * volume/mute; the SET paths are on_param_changed -> reac_headamp_prop_parse
	 * -> the pacer's head-amp command ring, and on_param_changed ->
	 * reac_rate_prop_parse -> reac_rate_cfg_decide -> reac_pacer_request_rate.
	 * Neither is echoed in the Props state object below: head-amp is
	 * write-through control re-asserted on the wire by the DMX scheduler, and
	 * rate reads back on reac.rate/reac.cfg.rate.* node properties instead
	 * (sink_publish_rate_props) — the same split link-state already uses. */
	params[3] = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
		SPA_PROP_INFO_id,          SPA_POD_Id(SPA_PROP_params),
		SPA_PROP_INFO_description, SPA_POD_String(
			"REAC head-amp: \"reac.headamp.<ch>.{phantom,pad,sens}\" = value; "
			"REAC rate: \"reac.cfg.rate\" = 44100|48000|96000; "
			"REAC role: \"reac.cfg.role\" = 0 (master) | 1 (slave)"),
		SPA_PROP_INFO_type,        SPA_POD_String(
			"reac.headamp.<ch>.<param> | reac.cfg.rate | reac.cfg.role"));

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
	if (!n->stream)
		return;
	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[5];
	uint32_t np = sink_build_params(n, &b, params);
	pw_stream_update_params(n->stream, params, np);
}

/* MAIN LOOP: a controller changed our node params. We only care about node-global
 * Props (port_data == NULL). Parse volume / mute / channelVolumes and re-publish.
 * channelVolumes is authoritative per-channel; a bare `volume` scalar sets all
 * channels (so both a mono and a per-channel controller work, with no double
 * count). Values are linear (reac_gain.h); negatives clamp to silence. */
static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct reac_sink_node *n = data;
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

	/* LIVE rate control (2026-08-26-reac-runtime-config.md): the same Props
	 * object may carry a `reac.cfg.rate` assertion under SPA_PROP_params. The
	 * DECISION (reac_rate_cfg_decide) is pure and runs right here on the main
	 * loop; only an ACCEPTED rate crosses to the RT pacer thread
	 * (reac_pacer_request_rate), which is the only thing that actually needs
	 * to run there (period_ns/fps/the master FSM are pacer-thread-owned state,
	 * same reasoning as the head-amp table above). This node exists ONLY in
	 * the master role (reac_sink_node_new is never called for a slave), so
	 * REAC_ROLE_MASTER is a fact of this call site, not a read of some stored
	 * role — a slave's own REFUSE_ROLE_SLAVE answer is exercised at the
	 * decision-core level (test_reac_rate_cfg.c), because a slave has no
	 * props-carrying node to assert it through at all yet. */
	int req_hz;
	int rate_parsed = reac_rate_prop_parse(param, &req_hz);
	if (rate_parsed != 0) {
		enum reac_rate_refuse refusal = rate_parsed < 0
			? REAC_RATE_REFUSE_MALFORMED
			: reac_rate_cfg_decide(REAC_ROLE_MASTER, req_hz, n->pacer.drivable_mask);
		atomic_store_explicit(&n->pacer.rate_refused, (int)refusal, memory_order_relaxed);
		if (refusal == REAC_RATE_REFUSE_NONE)
			reac_pacer_request_rate(&n->pacer, req_hz);
		/* A refusal moves nothing: no request reaches the pacer, so fps,
		 * period_ns and the master FSM are untouched — the refused prop
		 * above is the only thing that changes. */
	}

	/* LIVE role control (2026-08-26-reac-runtime-config.md, the ROLE half):
	 * the same Props object may carry a `reac.cfg.role` assertion under
	 * SPA_PROP_params. This node exists ONLY in the master role
	 * (reac_sink_node_new is never called for a slave), so REAC_ROLE_MASTER
	 * is a fact of this call site exactly as it is for rate above — a
	 * slave's own answer is exercised at the decision-core level
	 * (test_reac_role_cfg.c), because a slave has no props-carrying node to
	 * assert it through at all yet.
	 *
	 * Unlike rate, applying the decision never reaches the pacer/RT thread:
	 * asserting the role we already are is a genuine no-op answered
	 * "applied"; asserting the other role is accepted as well-formed but the
	 * cross-engine swap it would take (tear down this master engine, bring
	 * up a slave one) is not performed here — see reac_role_cfg.h's HONESTY
	 * note for why, and REAC_ROLE_STATE_REESTABLISH_PENDING for the answer
	 * that says so instead of a fake "applied". */
	enum reac_role req_role;
	int role_parsed = reac_role_prop_parse(param, &req_role);
	if (role_parsed != 0) {
		n->role_refused = role_parsed < 0 ? REAC_ROLE_REFUSE_MALFORMED
		                                  : REAC_ROLE_REFUSE_NONE;
		if (n->role_refused == REAC_ROLE_REFUSE_NONE)
			n->role_state = reac_role_cfg_apply_state(REAC_ROLE_MASTER, req_role);
		/* A refusal leaves role_state exactly as it was: the malformed write
		 * changed nothing about the running role, so its answer should not
		 * look like it did either. */
	}

	if (changed)
		sink_publish(n);
}

/* The stream hands us its SPA_IO areas as it is configured. The sink needs
 * SPA_IO_Position for two things the filter got as a process() argument: the graph
 * quantum (which sizes the pacer's depth guard) and the driver's clock, which is the
 * graph-clock reference the #75 bridge grades and publishes. */
static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct reac_sink_node *n = data;
	if (id == SPA_IO_Position)
		n->position = (size >= sizeof(struct spa_io_position)) ? area : NULL;
	/* THE SINK HAD NO RATE MATCHING AT ALL, and that is why a discard was the only
	 * place the drift could go. The source node has published io_rate_match since
	 * the Tier-A clock bridge landed; the sink published nothing, so when the graph
	 * handed us more audio per second than we could put on the wire, the frame ring
	 * grew until the depth guard cut 256 frames out of it in one step. PipeWire's
	 * resampler is already in this node's adapter — the graph runs 192 kHz and the
	 * node runs 48 — and it will absorb a rate error continuously if we tell it
	 * one. It cannot be told by a node that never claims a rate.
	 *
	 * NULL when the link carries no resampler (a same-rate graph). We then publish
	 * "n/a" rather than a zero, because a correction that is not being applied and
	 * a correction of zero are different facts. */
	if (id == SPA_IO_RateMatch)
		n->rate_match = (size >= sizeof(struct spa_io_rate_match)) ? area : NULL;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.io_changed = on_io_changed,
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
	if (!n->stream)
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

	/* Head-amp preamp count follows the recognized model's INPUT width (each box
	 * input is a mic preamp); "0" until a model is recognized, mirroring the
	 * box-width "0x0" seed. The `caps` key is a constant seeded at create, so it
	 * is not re-stamped here (update_properties merges — untouched keys persist). */
	char ha_channels[16];
	snprintf(ha_channels, sizeof ha_channels, "%d", bm ? bm->in_ch : 0);

	/* The head-amp BASE, read from what the master published. It USED to be
	 * derived here by running the grant allocator over the recognized model's
	 * width, on the reasoning that the allocator was pure and total for a matrix
	 * width — so calling it was "reading the master's decision, not re-deciding
	 * it". That reasoning is retired: the base was never the master's decision.
	 * It is the box's own chassis strap, announced on the wire, and no function
	 * of the width can return it for a chassis whose strap and width are not
	 * collinear. It now crosses on its own atomic because it genuinely cannot be
	 * recomputed from anything else on this side. -1 means no box. */
	char ha_base[16];
	int base = atomic_load_explicit(&n->pacer.recognized_headamp_base,
	                                memory_order_acquire);
	if (base >= 0)
		snprintf(ha_base, sizeof ha_base, "%d", base);
	else
		snprintf(ha_base, sizeof ha_base, "%s", REAC_BOX_SOURCE_NONE);

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_LINK_STATE,      reac_link_state_name(ls),
		REAC_PROP_BOX_MODEL,       bm ? bm->token : "none",
		REAC_PROP_BOX_WIDTH,       width,
		REAC_PROP_BOX_SOURCE,      bm ? REAC_BOX_SOURCE_WIRE : REAC_BOX_SOURCE_NONE,
		REAC_PROP_HEADAMP_CHANNELS, ha_channels,
		REAC_PROP_HEADAMP_BASE,    ha_base,
		NULL);
	if (props) {
		pw_stream_update_properties(n->stream, &props->dict);
		pw_properties_free(props);
	}

	/* #208: keep the reac-capture (source) badge in lock-step with this playback side.
	 * Reached only when ls/bm CHANGED (the early-return above), which is exactly when
	 * the box establishes / drops / swaps — and a source rebuilt on a width change is a
	 * bm change, so it is always re-stamped here. Slot-deref follows the current node;
	 * same main loop, so this is thread-safe. */
	if (n->peer_src && *n->peer_src)
		reac_source_node_publish_link(*n->peer_src,
		                              reac_link_state_name(ls),
		                              bm ? bm->token : "none",
		                              width);
}

/* MAIN LOOP: force the live adapter to actually present `hz`, closing the
 * gap increment 3 left (docs/design/notes/2026-08-26-rate-change-node-format-
 * gap.md's correction). `pw_stream_update_params(EnumFormat)` on an already-
 * connected, streaming node advertises a new SUPPORTED set; it does not
 * renegotiate the ACTIVE format — measured live: the update call ran, `pw-
 * dump` Format.rate did not move. The robust trigger is the same shape as a
 * graph-side node re-establish: disconnect, then connect again with the
 * new-rate Format — the sink's equivalent of the wire's own re-establish the
 * pacer already performs in reac_pacer_apply_rate.
 *
 * SAME STREAM OBJECT, not a destroy+recreate. reac_sink_node_ensure's box-
 * width path destroys and calls sink_open_filter to build a brand new
 * pw_stream (a new node identity is correct there — the box itself changed).
 * A rate change is not that: the node should stay the SAME node (same id,
 * same reac.link-state/box-model/discovery properties, which live on the
 * stream object and are set once at pw_stream_new_simple — a destroy+
 * recreate would flash them back to the "probing"/"none" connect-time seed
 * for one registry update, which is honest for a box swap and dishonest
 * here). Disconnect+connect on the SAME object renegotiates only the Format/
 * Props params and leaves everything else on the node untouched.
 *
 * RT FEED SAFETY. on_process runs on the stream's own RT data thread
 * (PW_STREAM_FLAG_RT_PROCESS); PipeWire does not invoke it while the stream
 * is disconnected — the same guarantee reac_sink_node_ensure's destroy+
 * rebuild already depends on ("no RT race" — see reac_sink_node_ensure's own
 * comment). on_process also carries a belt-and-braces rate_reconnecting
 * check as its first line (set here, cleared below) for defense in depth.
 * Either way, a cycle skipped during the reconnect window costs nothing
 * dangerous: the pacer's frame ring is fed independently of this stream's
 * lifecycle and simply free-runs FILLER for the gap, exactly the existing
 * have == 0 fallback already does whenever nothing is linked.
 *
 * n->position / n->rate_match are cleared across the gap: they point at SPA_
 * IO areas the graph handed us via io_changed, and a fresh connect gets a
 * fresh (possibly different) area — reading the old pointer in the interim
 * would be reading a stale binding, not a use-after-free (PipeWire owns that
 * memory for the node's lifetime), but stale all the same until io_changed
 * fires again on the new connection.
 *
 * Returns 0 and adopts `hz` on success. On failure it re-asserts the OLD
 * rate (`prev_hz`, still known-good) so the sink does not end up silently
 * disconnected until some unrelated event (a box swap) happens to rebuild
 * it, and returns -1 — reac_sink_format_rate_after_attempt is what decides
 * n->sample_rate honestly either way. */
static int sink_reconnect_rate(struct reac_sink_node *n, int hz)
{
	if (!n->stream)
		return -1;
	int prev_hz = n->sample_rate;

	atomic_store_explicit(&n->rate_reconnecting, 1, memory_order_relaxed);
	pw_stream_disconnect(n->stream);
	n->position = NULL;
	n->rate_match = NULL;

	uint8_t fbuf[1024];
	struct spa_pod_builder fb = SPA_POD_BUILDER_INIT(fbuf, sizeof fbuf);
	uint8_t pbuf[2048];
	struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pbuf, sizeof pbuf);
	const struct spa_pod *cparams[5];
	uint32_t ncp = sink_build_params(n, &pb, cparams);

	const struct spa_pod *sparams[8];
	uint32_t nsp = 0;
	sparams[nsp++] = reac_sink_format_build(&fb, n->channels, hz);
	for (uint32_t i = 0; i < ncp && nsp < 8; i++)
		sparams[nsp++] = cparams[i];

	int ok = pw_stream_connect(n->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
	                          PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
	                          sparams, nsp) >= 0;
	if (!ok) {
		pw_log_warn("reac:playback — rate reconnect to %d Hz failed; "
		            "re-asserting %d Hz so the sink is not left disconnected",
		            hz, prev_hz);
		uint8_t fbuf2[1024];
		struct spa_pod_builder fb2 = SPA_POD_BUILDER_INIT(fbuf2, sizeof fbuf2);
		const struct spa_pod *fallback = reac_sink_format_build(&fb2, n->channels, prev_hz);
		const struct spa_pod *fsparams[8];
		uint32_t fnsp = 0;
		fsparams[fnsp++] = fallback;
		for (uint32_t i = 0; i < ncp && fnsp < 8; i++)
			fsparams[fnsp++] = cparams[i];
		if (pw_stream_connect(n->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
		                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
		                      fsparams, fnsp) < 0)
			pw_log_warn("reac:playback — fallback reconnect to %d Hz ALSO failed; "
			            "sink is disconnected until the next box/format event", prev_hz);
	}
	atomic_store_explicit(&n->rate_reconnecting, 0, memory_order_relaxed);

	/* link/box/disco properties live on the stream object across a same-
	 * object disconnect+connect (unlike sink_open_filter's destroy+recreate,
	 * this never resets their shadows, so they need no re-stamp here).
	 * ProcessLatency IS rate-dependent, but on_log_timer — this function's
	 * only caller — already calls sink_publish_latency right after
	 * sink_publish_rate_props on the same 200 ms tick, so the figure is
	 * re-derived for whichever rate we ended up presenting without a second
	 * call here. */
	n->sample_rate = reac_sink_format_rate_after_attempt(hz, prev_hz, ok);
	return ok ? 0 : -1;
}

/* MAIN LOOP: stamp reac.rate / reac.rate.source / reac.rate.drivable /
 * reac.cfg.rate.state / reac.cfg.rate.refused (2026-08-26-reac-runtime-
 * config.md §0/§1) — the read side of the `reac.cfg.rate` write door
 * on_param_changed answers below. Reads only the pacer's cross-thread-safe
 * rate atomics (never on_process/RT); re-stamps only when one of them
 * actually moved, same shadow-and-compare pattern as sink_publish_link_props.
 * drivable_mask is read-only after reac_pacer_open, so it needs no shadow —
 * it can only ever agree with itself. */
static void sink_publish_rate_props(struct reac_sink_node *n)
{
	if (!n->stream)
		return;

	int hz = atomic_load_explicit(&n->pacer.rate_hz, memory_order_acquire);
	int asserted = atomic_load_explicit(&n->pacer.rate_asserted, memory_order_relaxed);
	int reest = atomic_load_explicit(&n->pacer.rate_reestablishing, memory_order_acquire);
	enum reac_rate_refuse refused = (enum reac_rate_refuse)
		atomic_load_explicit(&n->pacer.rate_refused, memory_order_relaxed);

	if (hz == n->rate_hz_last && asserted == n->rate_asserted_last &&
	    reest == n->rate_reestablishing_last && refused == n->rate_refused_last)
		return;                          /* unchanged: do not spam the update */
	n->rate_hz_last = hz;
	n->rate_asserted_last = asserted;
	n->rate_reestablishing_last = reest;
	n->rate_refused_last = refused;

	/* Renegotiate the node's presented Format when the pacer's accepted rate has
	 * moved past what we last built/pushed — the exact gap docs/design/notes/
	 * 2026-08-26-rate-change-node-format-gap.md measured: `reac_pacer_apply_rate`
	 * re-clocks the WIRE, but a bare `pw_stream_update_params(EnumFormat)` never
	 * renegotiated the pw_stream node's ACTIVE format, so pw-top kept reading the
	 * boot rate (increment 3, measured not-working live). sink_reconnect_rate is
	 * increment 4's fix: a disconnect+connect re-establish, the trigger that
	 * actually forces it. MAIN LOOP only, same as this whole function —
	 * pw_stream_disconnect/connect are never safe off the loop thread, which is
	 * why this reads the pacer's rate_hz ATOMIC on the timer poll rather than
	 * being invoked FROM the SCHED_FIFO pacer thread that set it (the same
	 * cross-thread pattern sink_publish_link_props/_disco_props already use).
	 * reac_sink_format_needs_update is false on a normal single-rate boot (n-
	 * >sample_rate was seeded from the very same rate at construction) and false
	 * again once sink_reconnect_rate has caught the node up, so a boot or an
	 * already-caught-up node never reconnects at all — no churn where nothing
	 * moved. n->sample_rate is updated INSIDE sink_reconnect_rate, honestly
	 * (reac_sink_format_rate_after_attempt), only once the attempt's outcome is
	 * known — never optimistically ahead of what pw_stream_connect actually did. */
	if (reac_sink_format_needs_update(n->sample_rate, hz))
		sink_reconnect_rate(n, hz);

	/* ONE WIRE, ONE RATE (2026-08-26-clock-tabs-and-reac-pace-coupling.md §1b):
	 * push the same accepted rate onto the peer reac-capture node so it
	 * presents the same Format the wire is actually running at — the gap that
	 * left reac-capture's Format stuck at boot rate while reac-playback's
	 * followed. This is not a second decision: `hz` (the pacer's rate_hz) IS
	 * the wire rate for a master, and peer_src is wired only in the master
	 * role (main.c calls reac_sink_node_set_peer_source only for c->role ==
	 * REAC_ROLE_MASTER), so this call is reached only where that fact holds.
	 * reac_source_node_publish_rate is itself idempotent (its own reac_sink_
	 * format_needs_update check), so calling it here — inside the branch that
	 * already gates on THIS node's rate having moved — costs nothing extra on
	 * a poll where nothing changed. */
	if (n->peer_src && *n->peer_src)
		reac_source_node_publish_rate(*n->peer_src, hz);

	char rate_s[16];
	snprintf(rate_s, sizeof rate_s, "%d", hz);
	char drivable[32];
	reac_rate_drivable_csv(n->pacer.drivable_mask, drivable, sizeof drivable);

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_RATE,          rate_s,
		REAC_PROP_RATE_SOURCE,   asserted ? REAC_RATE_SOURCE_ASSERTED : REAC_RATE_SOURCE_CONVENTION,
		REAC_PROP_RATE_DRIVABLE, drivable,
		REAC_PROP_RATE_STATE,    reest ? REAC_RATE_STATE_PENDING : REAC_RATE_STATE_APPLIED,
		REAC_PROP_RATE_REFUSED,  reac_rate_refuse_code(refused),
		NULL);
	if (props) {
		pw_stream_update_properties(n->stream, &props->dict);
		pw_properties_free(props);
	}
}

/* MAIN LOOP: stamp reac.role / reac.cfg.role.state / reac.cfg.role.refused
 * (2026-08-26-reac-runtime-config.md, the ROLE half) — the read side of the
 * `reac.cfg.role` write door on_param_changed answers below. Plain-field
 * shadow-and-compare, same pattern as sink_publish_rate_props above; no
 * cross-thread atomic to read because this increment's role apply never
 * reaches the pacer/RT thread at all (reac_role_cfg.h's HONESTY note).
 *
 * reac.role reports the FACT that this node is running, not the console's
 * latest reac.cfg.role request: this node exists ONLY in the master role
 * (reac_sink_node_new is never called for a slave), and the cross-engine
 * swap a role-changing assertion would need is exactly what this increment
 * does not perform — so the observed role stays master, honestly, even while
 * reac.cfg.role.state says a change is pending. Publishing anything else
 * here would be the "fake success" this module's header explicitly refuses
 * to produce. */
static void sink_publish_role_props(struct reac_sink_node *n)
{
	if (!n->stream)
		return;

	if (n->role_state == n->role_state_last && n->role_refused == n->role_refused_last)
		return;                          /* unchanged: do not spam the update */
	n->role_state_last = n->role_state;
	n->role_refused_last = n->role_refused;

	char role_s[4];
	snprintf(role_s, sizeof role_s, "%d", REAC_CFG_ROLE_VALUE_MASTER);

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_ROLE,         role_s,
		REAC_PROP_ROLE_STATE,   n->role_state,
		REAC_PROP_ROLE_REFUSED, reac_role_refuse_code(n->role_refused),
		NULL);
	if (props) {
		pw_stream_update_properties(n->stream, &props->dict);
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
	if (!n->stream)
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

	/* THE SEGMENT AGGREGATE, computed from the same sightings and our own FSM state, and
	 * published in the SAME update as them — the atomicity the spec asks for is simply that
	 * they move together with the seq. Passive: reac_arbitrate decides nothing, and nothing
	 * downstream acts on it yet.
	 *
	 * The pace source is what we ARE running on, not what we would prefer: with clock-follow
	 * off (the config of record after the 2026-08-21 verdict) that is free-run, and saying
	 * "graph-ref" because the code exists would be the same lie as a soft meter. */
	struct reac_arbitration arb;
	reac_arbitrate(&n->pacer.disco, n->pacer.master.src, n->pacer.master.state,
	               REAC_PACE_FREE_RUN, reac_pacer_mono_ns(), &arb);

	char master_mac[24];
	if (arb.have_mac)
		snprintf(master_mac, sizeof master_mac, "%02x:%02x:%02x:%02x:%02x:%02x",
		         arb.mac[0], arb.mac[1], arb.mac[2], arb.mac[3], arb.mac[4], arb.mac[5]);
	else
		snprintf(master_mac, sizeof master_mac, "none");

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_DISCO_SCOPE,   n->disco_ifname ? n->disco_ifname : "",
		REAC_PROP_DISCO_STATE,   REAC_DISCO_STATE_LISTENING,
		REAC_PROP_DISCO_SEQ,     seq,
		REAC_PROP_DISCO_DEVICES, devices,
		REAC_PROP_MASTER_STATE,  reac_segment_master_name(arb.state),
		REAC_PROP_MASTER_MAC,    master_mac,
		REAC_PROP_PACE_SOURCE,   reac_pace_source_name(arb.pace),
		REAC_PROP_MASTER_CONFLICT, arb.conflict ? "1" : "0",
		NULL);
	if (props) {
		pw_stream_update_properties(n->stream, &props->dict);
		pw_properties_free(props);
		n->disco_seq_last = n->pacer.disco.seq;
	}
}

/* MAIN LOOP: publish the daemon's own health onto the node, so the console can
 * see the fault it is otherwise structurally unable to see.
 *
 * THE 64 ms DISCARD RAISES NO XRUN. That is not an oversight in the guard, it is
 * the guard's purpose: PipeWire never starves, so no xrun counter moves, no
 * telemetry row changes, and the console draws a healthy graph while audio goes
 * missing in 64 ms blocks. Every other symptom of this fault is also absent by
 * construction. So the daemon says it in numbers, on the one doorway it already
 * has to the console — its own node properties, beside reac.link-state.
 *
 * Published only when a health window closes (10 s), and every value is a rate
 * over that window, so a consumer that skips updates loses resolution and nothing
 * else. That is what openmixer's telemetry contract asks for: its own SSE,
 * latest-wins, skip when late, never accumulate. */
static void sink_publish_health(struct reac_sink_node *n)
{
	if (!n->stream || !n->pacer_open)
		return;
	if (!reac_pacer_health_poll(&n->pacer, reac_pacer_mono_ns(), &n->health))
		return;   /* window still open */

	const struct reac_pacer_health *h = &n->health;
	char drift[24], dfps[24], dms[24], txe[24], lw[24], lwps[24];
	char cups[24], drps[24], rfr[24], rms[24], rmatch[24], dmax[24];
	snprintf(drift,  sizeof drift,  "%.1f", h->drift_ppm);
	snprintf(dfps,   sizeof dfps,   "%.3f", h->discard_fps);
	snprintf(dms,    sizeof dms,    "%.3f", h->discard_ms_per_s);
	snprintf(txe,    sizeof txe,    "%llu", (unsigned long long)h->tx_errors);
	snprintf(lw,     sizeof lw,     "%llu", (unsigned long long)h->late_wakes);
	snprintf(lwps,   sizeof lwps,   "%.2f", h->late_wakes_ps);
	snprintf(cups,   sizeof cups,   "%.2f", h->slots_catchup_ps);
	snprintf(drps,   sizeof drps,   "%.2f", h->slots_dropped_ps);
	snprintf(rfr,    sizeof rfr,    "%u",   h->ring_frames);
	snprintf(dmax,   sizeof dmax,   "%u",   h->slot_debt_max);
	snprintf(rms,    sizeof rms,    "%.2f", h->ring_ms);
	/* "n/a" is not decoration. A link with no resampler gives us no rate-match
	 * area, and reporting 0 there would claim we are steering something we cannot
	 * reach — the same lie as a soft meter reading a hardware state. */
	if (atomic_load_explicit(&n->rate_match_present, memory_order_relaxed))
		snprintf(rmatch, sizeof rmatch, "%.1f",
		         atomic_load_explicit(&n->rate_match_milli_ppm,
		                              memory_order_relaxed) / 1000.0);
	else
		snprintf(rmatch, sizeof rmatch, "n/a");

	struct pw_properties *props = pw_properties_new(
		REAC_PROP_HEALTH_DRIFT_PPM,   drift,
		REAC_PROP_HEALTH_DISCARD_FPS, dfps,
		REAC_PROP_HEALTH_DISCARD_MS,  dms,
		REAC_PROP_HEALTH_TX_ERRORS,   txe,
		REAC_PROP_HEALTH_LATE_WAKES,  lw,
		REAC_PROP_HEALTH_LATE_PS,     lwps,
		REAC_PROP_HEALTH_CATCHUP_PS,  cups,
		REAC_PROP_HEALTH_DROPPED_PS,  drps,
		REAC_PROP_HEALTH_DEBT_MAX,    dmax,
		REAC_PROP_HEALTH_RING_FRAMES, rfr,
		REAC_PROP_HEALTH_RING_MS,     rms,
		REAC_PROP_HEALTH_RATE_MATCH,  rmatch,
		NULL);
	if (props) {
		pw_stream_update_properties(n->stream, &props->dict);
		pw_properties_free(props);
	}

	/* The same window on stderr, because the journal is where a fault is read
	 * after the fact and a property only ever shows the latest value. */
	fprintf(stderr,
	        "reac-health: drift %+.1f ppm | discard %.3f frames/s (%.3f ms/s) | "
	        "ring %u frames (%.2f ms) | late %.2f/s (catchup %.2f/s, dropped %.2f/s, "
	        "worst debt %u slots) | tx_errors %llu | rate-match %s ppm\n",
	        h->drift_ppm, h->discard_fps, h->discard_ms_per_s,
	        h->ring_frames, h->ring_ms, h->late_wakes_ps,
	        h->slots_catchup_ps, h->slots_dropped_ps, h->slot_debt_max,
	        (unsigned long long)h->tx_errors, rmatch);
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
	if (!n->stream)
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
	pw_stream_update_params(n->stream, &param, 1);
}

/* MAIN LOOP (non-RT): drain the pacer's FSM event ring to stderr, then
 * re-stamp the link-state node properties from the same non-RT cadence. The
 * pacer thread is SCHED_FIFO and must not touch stdio (or PipeWire API); it
 * logs into a lock-free ring and this 200 ms timer formats it — so a live
 * power-cycle prints the complete establishment transcript (presence edges,
 * JOIN hex dumps, transitions) AND keeps reac.link-state live. */
/* BOX CLOCK REFERENCE (#75). MAIN LOOP only — no RT path touched.
 *
 * The measurement already exists: reac_rx is the rate authority and tracks the
 * box's byte-14/15 counter slope against CLOCK_MONOTONIC, publishing a filtered
 * ppm error. We publish that as a candidate reference rather than adding a second
 * estimator.
 *
 * This is the case the issue exists to encode: reac-pw is the REAC MASTER — it
 * grants, it drives the segment — while being a clock FOLLOWER, because the
 * stagebox is fed from a house word clock and is the rig's clock master. REAC
 * master and clock master are different roles.
 *
 * It is also safe in the ordinary case where the box is slaved to US. Such a box
 * recovers its word clock from our cadence, so its counter slope measured against
 * the host clock is exactly our own applied correction handed back: the loop's
 * residual is identically zero, it parks where it is, and the cadence cannot run
 * away (pinned in test_reac_pacer_clock).
 *
 * Present only once the master is ESTABLISHED with a box AND reac_rx has actually
 * closed a slope window — ppm_error_milli reads 0 both before the first estimate
 * and when the slope is genuinely zero, and steering off the former would be
 * mistaking "no information" for "the reference agrees with us". */
static void sink_publish_box_clock(struct reac_sink_node *n)
{
	if (!n->pacer.clock_follow || !n->rate_src)
		return;
	uint32_t seq = atomic_load_explicit(&n->rate_src->ppm_seq, memory_order_acquire);
	int established = atomic_load_explicit(&n->pacer.fsm_state,
	                                       memory_order_relaxed) == REAC_M_ESTABLISHED;
	if (!established || seq == 0 || seq == n->box_ppm_seq) {
		/* Not established, no estimate yet, or no NEW estimate since the last
		 * poll. Withdraw rather than re-vouch for a stale number; the pacer's
		 * staleness ageing would catch a silent publisher anyway, this is just the
		 * earlier and more explicit half of the same honesty. */
		if (!established || seq == 0)
			reac_pacer_clock_publish(&n->pacer, REAC_CLOCK_SRC_BOX, 0, 0, NULL,
			                         REAC_CLOCK_Q_UNGRADED, 0);
		return;
	}
	n->box_ppm_seq = seq;
	const struct reac_box_model *bm =
		atomic_load_explicit(&n->pacer.recognized_box, memory_order_acquire);
	const char *label = bm ? bm->display : "box";
	/* A stagebox is a stagebox: the name heuristic has nothing to say about one
	 * (it grades UNGRADED, as intended), but the operator CAN designate a box
	 * that is itself the segment's clock master — the case the issue calls a
	 * first-class alternative to the RME. Measurement never promotes this tier;
	 * see the closed-loop asymmetry in reac_clock.h. */
	reac_pacer_clock_publish(&n->pacer, REAC_CLOCK_SRC_BOX, 1,
	                         atomic_load_explicit(&n->rate_src->ppm_error_milli,
	                                              memory_order_relaxed),
	                         label, reac_clock_grade_name(label, n->clock_ref),
	                         reac_pacer_mono_ns());
}

static void on_log_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct reac_sink_node *n = data;
	/* THE RX FOLLOWS THE DECLARED BOX. The upstream gate latches the first
	 * box-shaped source it sees and had no way back out, so after a hot swap it
	 * kept decoding the departed box's MAC: every frame from the new box failed
	 * the compare, frames_ok stopped advancing, and reac-capture published
	 * silence while the wire carried a live microphone (rig 2026-08-21, S-1608
	 * out / S-0808 in — the box synced, the graph was patched, and MAIN measured
	 * digital silence). Box identity belongs to the MASTER; the gate mirrors it
	 * rather than keeping a second, older opinion. Idempotent, so it costs a
	 * compare per tick once they agree. */
	/* Idempotent backstop only. The reset that MATTERS happens in
	 * note_transition, the instant the session changes; this catches a receiver
	 * attached after a transition and costs one compare once they agree. */
	if (n->rate_src && reac_master_has_box(&n->pacer.master))
		reac_rx_peer_reset(n->rate_src, n->pacer.master.box_mac,
		                   n->pacer.master.session_seq);
	sink_publish_box_clock(n);     /* before the drain, so a change prints now */
	reac_pacer_log_drain(&n->pacer, stderr);
	sink_publish_link_props(n);
	sink_publish_rate_props(n);
	sink_publish_role_props(n);
	sink_publish_disco_props(n);   /* strictly AFTER the drain: it builds pacer.disco */
	sink_publish_latency(n);
	sink_publish_health(n);
}

/* Build the node DESCRIPTION for `channels` outputs labelled `label` (NULL/"" ->
 * the role-default text). Single formatter used at (re)build AND relabel. */
static void sink_build_desc(char *desc, size_t sz, const char *label, int channels)
{
	if (label && *label)
		snprintf(desc, sz, "%s — %d ch (REAC box outputs)", label, channels);
	else
		snprintf(desc, sz, "REAC %dch playback (downstream master TX)", channels);
}

/* Build (or rebuild) the reac-playback pw_filter at n->channels INPUT ports
 * labelled `label`, connect it, and stamp the live badge props onto the fresh
 * node. THE PACER IS NOT TOUCHED — this manages only the graph filter, so a resize
 * never disturbs the running master/recognizer. On a rebuild the caller has already
 * destroyed the old filter and nulled n->ports; the badge-prop shadows are reset to
 * the create-time seeds here and immediately re-published from the pacer snapshot,
 * so a rebuilt node shows the live link-state/box-model/discovery/latency at once
 * (not only after the next 200 ms poll). Returns 0, or -1 (n->filter left NULL). */
static int sink_open_filter(struct reac_sink_node *n, const char *label)
{
	char rate_str[16];
	snprintf(rate_str, sizeof rate_str, "1/%d", n->sample_rate);

	char nodename[64];
	if (n->inst && *n->inst)
		snprintf(nodename, sizeof nodename, "reac-playback.%s", n->inst);
	else
		snprintf(nodename, sizeof nodename, "reac-playback");

	snprintf(n->label, sizeof n->label, "%s", label ? label : "");
	char desc[128];
	sink_build_desc(desc, sizeof desc, n->label[0] ? n->label : NULL, n->channels);

	n->stream = pw_stream_new_simple(
		n->loop,
		"reac:playback",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback", /* a sink consumes audio */
			PW_KEY_MEDIA_CLASS, "Audio/Sink",  /* shows up as an output device */
			PW_KEY_NODE_NAME, nodename,
			PW_KEY_NODE_DESCRIPTION, desc,
			/* NO node.rate: on a filter that was a REQUEST for the graph to run at
			 * the REAC rate, which an RME-driven graph refuses. The rate that matters
			 * is the one in our FORMAT, which the adapter resamples from. */
			/* Correct-at-(re)build badge props (task #154): seeded to the "probing/
			 * none/0x0" baseline and immediately re-stamped from the pacer below.
			 * Kept live by sink_publish_link_props on the 200 ms log-timer. */
			REAC_PROP_LINK_STATE, reac_link_state_name(REAC_LINK_PROBING),
			REAC_PROP_BOX_MODEL, "none",
			REAC_PROP_BOX_WIDTH, "0x0",
			REAC_PROP_BOX_SOURCE, REAC_BOX_SOURCE_NONE,
			REAC_PROP_HEADAMP_BASE, REAC_BOX_SOURCE_NONE,
			/* Head-amp CAPABILITIES (task #205), published on THIS node because it
			 * is the one that consumes the reac.headamp.<ch>.<param> control keys
			 * (on_param_changed -> reac_headamp_prop_parse), so a consumer sees the
			 * box's preamp shape and drives it on ONE node. `channels` seeds "0" and
			 * is bumped to the model's input width by sink_publish_link_props on
			 * recognition; `caps` is the constant phantom/pad/sens trio. */
			REAC_PROP_HEADAMP_CHANNELS, "0",
			REAC_PROP_HEADAMP_CAPS, REAC_HEADAMP_CAPS_DEFAULT,
			/* Correct-at-(re)build discovery (task #178): from this node's t=0 we are
			 * listening on this NIC; seq "0"/"[]" is re-stamped from the pacer's disco
			 * table below. Kept live by sink_publish_disco_props on the log-timer. */
			REAC_PROP_DISCO_SCOPE, n->disco_ifname ? n->disco_ifname : "",
			REAC_PROP_DISCO_STATE, REAC_DISCO_STATE_LISTENING,
			REAC_PROP_DISCO_SEQ, "0",
			REAC_PROP_DISCO_DEVICES, "[]",
			NULL),
		&stream_events, n);
	if (!n->stream)
		return -1;

	/* Shadows to the seeds just published; then re-publish from the live pacer state
	 * so a (re)built node converges within this call rather than after a 200 ms poll.
	 * link_drops_seen tracks the CURRENT cumulative drops so the rebuild does not
	 * flash a spurious "dropped" overlay. */
	n->link_state_last = REAC_LINK_PROBING;
	n->box_model_last = NULL;
	n->disco_seq_last = 0;
	n->link_drops_seen = 0;
	for (int i = 0; i < 8; i++)
		n->link_drops_seen += atomic_load_explicit(&n->pacer.drops[i],
		                                            memory_order_relaxed);
	reac_lat_init(&n->lat);

	/* ONE format, not one port per channel — the adapter builds the ports from it.
	 * F32 PLANAR keeps the stage's layout; AUX0..AUXN marks each box output as a
	 * DISCRETE mono send so no graph tool pairs them as stereo. The rate is THE REAC
	 * PACE, and PipeWire resamples the graph's pace into it: that is the whole point
	 * of being an adapter, and it is what removes the ring-trim discards. Built by
	 * reac_sink_format_build so a later renegotiation (sink_publish_rate_props,
	 * #4.3) shares this exact shape rather than a second hand-copied one. */
	uint8_t fbuf[1024];
	struct spa_pod_builder fb = SPA_POD_BUILDER_INIT(fbuf, sizeof fbuf);

	/* Advertise the volume/mute PropInfo + the current (persisted) Props at connect,
	 * so a controller sees the controls the moment the node appears and standard
	 * volume tools drive the box outputs (the raw filter has no audioadapter, so
	 * without this wpctl/desktop volume would be silently ignored). */
	uint8_t pbuf[2048];
	struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pbuf, sizeof pbuf);
	const struct spa_pod *cparams[5];
	uint32_t ncp = sink_build_params(n, &pb, cparams);

	const struct spa_pod *sparams[8];
	uint32_t nsp = 0;
	sparams[nsp++] = reac_sink_format_build(&fb, n->channels, n->sample_rate);
	for (uint32_t i = 0; i < ncp && nsp < 8; i++)
		sparams[nsp++] = cparams[i];
	if (pw_stream_connect(n->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
	                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
	                      sparams, nsp) < 0) {
		pw_stream_destroy(n->stream);
		n->stream = NULL;
		return -1;
	}

	/* Stamp the live badges + graph->wire latency onto the fresh node now (the
	 * shadows above were reset to the seeds, so these publish the current pacer
	 * state immediately). */
	sink_publish_link_props(n);
	sink_publish_rate_props(n);
	sink_publish_role_props(n);
	sink_publish_disco_props(n);
	sink_publish_latency(n);
	return 0;
}

struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg)
{
	(void)tx_ring; /* the pacer owns its own frame ring (reac_pacer.ring) */

	struct reac_sink_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->loop = loop;
	n->inst = cfg->inst;          /* stable for the process; used by every filter build */
	n->disco_ifname = cfg->ifname;
	n->channels = 0;              /* no graph filter yet — DEFERRED to reac_sink_node_ensure */
	n->sample_rate = cfg->sample_rate;
	snprintf(n->label, sizeof n->label, "%s", cfg->label ? cfg->label : "");

	/* Output gain starts at UNITY (calloc would leave it 0 == fully muted). The
	 * ramp step is one REAC_GAIN_RAMP_MS worth of samples at the wire rate; a
	 * safe fallback keeps it positive if the rate is somehow unset. Persisted across
	 * a filter rebuild (indexed by channel, so a resize keeps each channel's gain). */
	float ramp_samples = REAC_GAIN_RAMP_MS * (float)n->sample_rate / 1000.0f;
	n->ramp_step = ramp_samples > 1.0f ? 1.0f / ramp_samples : 1.0f;
	n->muted = false;
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
		n->chan_vol[c] = 1.0f;
		n->chan_cur[c] = 1.0f;
		atomic_init(&n->chan_target[c], 1.0f);
	}
	atomic_init(&n->rate_reconnecting, 0);

	/* reac.cfg.role's standing answer: nothing has been asserted yet, so the
	 * fact is simply "applied" (we are already what we are) with no refusal.
	 * *_last is seeded to values the real answer can never equal (calloc left
	 * role_state_last NULL, which no REAC_ROLE_STATE_* string pointer is, and
	 * -1 is not a valid enum reac_role_refuse) so sink_publish_role_props's
	 * first call always publishes rather than reading a coincidental match. */
	n->role_state = REAC_ROLE_STATE_APPLIED;
	n->role_refused = REAC_ROLE_REFUSE_NONE;
	n->role_refused_last = (enum reac_role_refuse)-1;

	/* Our master src MAC. The caller (main.c master path) supplies the impersonated
	 * desk's MAC; absent that, derive the Roland-OUI + this-NIC's-host-part default
	 * from the one helper (reac_mac.h) rather than a scattered hard-coded host part. */
	if (cfg->src_mac)
		memcpy(n->src, cfg->src_mac, 6);
	else
		reac_mac_default_src(cfg->ifname, n->src);

	/* The pacer is the master + the cadence clock + the box RECOGNIZER. fps = rate/12
	 * (downstream is 12 samples/frame at every rate). It opens the AF_PACKET TX socket
	 * and, once started, probes + establishes + identifies the box on the wire — all
	 * INDEPENDENT of the graph filter, so recognition works before any node exists. */
	struct reac_pacer_cfg pcfg = {
		.ifname = cfg->ifname,
		.fps = n->sample_rate / REAC_SAMPLES_PER_PKT,
		.prio = 0,        /* default 79 */
		.cpu = -1,        /* no pin by default (set on a dedicated rig host) */
		.src_mac = n->src,
		/* The master's OWN identity, and NO BOX (2026-08-05). The box comes from
		 * the wire; the console_field is the emulated mixer model, from the
		 * --mixer profile (default V-Mixer). This line used to read
		 * REAC_CONSOLE_CFG_S1608, whose in_channels=16 was the real origin of
		 * every head-amp slot address until a box was recognized — a hard-coded
		 * box, reachable from no flag and visible in no log. */
		.console = REAC_CONSOLE_CFG_IDLE,
	};
	pcfg.console.console_field = cfg->console_field;
	pcfg.headamps = cfg->headamps;        /* master head-amp DMX table (may be NULL) */
	pcfg.n_headamps = cfg->n_headamps;
	pcfg.clock_follow = cfg->clock_follow;   /* #75; 0 = free-run exactly as before */
	pcfg.rate_asserted = cfg->rate_asserted; /* the source label starts truthful */
	pcfg.catchup_max_slots = cfg->catchup_max_slots;  /* 0 = the measured default */
	n->rate_match_off = cfg->rate_match_off != 0;
	if (cfg->clock_ref) {                    /* #77; "" = designate nothing */
		strncpy(n->clock_ref, cfg->clock_ref, sizeof n->clock_ref - 1);
		n->clock_ref[sizeof n->clock_ref - 1] = '\0';
	}
	if (reac_pacer_open(&n->pacer, &pcfg) != 0) {
		pw_log_warn("reac:playback — cannot open AF_PACKET TX on '%s' "
		            "(needs CAP_NET_RAW + a valid interface); sink not created",
		            cfg->ifname);
		free(n);
		return NULL;
	}
	n->pacer_open = 1;

	/* Badge-prop shadows for the (yet-to-exist) filter. Seeded to the baseline so the
	 * first sink_open_filter re-stamps to the live pacer state. */
	n->link_state_last = REAC_LINK_PROBING;
	n->box_model_last = NULL;
	n->disco_seq_last = 0;
	n->link_drops_seen = 0;
	reac_lat_init(&n->lat);

	/* Start the SCHED_FIFO pacer thread. The master FSM probes immediately and
	 * unconditionally (a real unlinked M-5000 always hunts) and only GRANTS on
	 * the box's own cold-connect — no presence assumption, no timer advance
	 * (defect #130). The graph node is created later (on recognition), but the
	 * pacer free-runs silent FILLER meanwhile, so establishment is unaffected. */
	if (reac_pacer_start(&n->pacer) != 0) {
		pw_log_warn("reac:playback — cannot start cadence pacer thread");
		reac_pacer_close(&n->pacer);
		free(n);
		return NULL;
	}

	/* The FSM/RX log drain: 200 ms period on the main loop we already hold. It also
	 * keeps the badge/discovery/latency props live once a filter exists (it no-ops
	 * on n->filter == NULL, so it is safe before recognition). */
	n->log_timer = pw_loop_add_timer(loop, on_log_timer, n);
	if (n->log_timer) {
		struct timespec first = { 0, 200 * 1000000L };
		struct timespec interval = { 0, 200 * 1000000L };
		pw_loop_update_timer(loop, n->log_timer, &first, &interval, false);
	}

	pw_log_info("reac:playback MASTER engine on '%s' (%d Hz, %d fps pacer) — probing; "
	            "the reac-playback graph node appears sized to the box on recognition",
	            cfg->ifname, n->sample_rate, n->sample_rate / REAC_SAMPLES_PER_PKT);
	return n;
}

int reac_sink_node_ensure(struct reac_sink_node *n, int channels, const char *label)
{
	if (!n)
		return -1;
	int want = channels > REAC_MAX_CHANNELS ? REAC_MAX_CHANNELS : channels;
	if (want < 1)
		return -1;
	char want_label[64];
	snprintf(want_label, sizeof want_label, "%s", label ? label : "");
	if (n->stream && n->channels == want && strcmp(want_label, n->label) == 0)
		return 0;   /* identical box (same width AND label): nothing to do */
	/* Absent, or a box change — either a different width OR a same-out-width swap that
	 * only changes the label (e.g. S-1608 -> S-4000S, both 8 out). Either way REBUILD
	 * the graph filter: pw_filter_update_properties does NOT re-stamp a live node's
	 * node.description / reac.box-model / discovery props to the registry (they stay at
	 * their connect-time values — verified on the rig), so relabelling a connected
	 * filter is not possible; only a fresh filter's CREATION-time props propagate.
	 * sink_open_filter re-stamps all of them. The pacer/recognizer is UNTOUCHED
	 * (pw_filter_destroy quiesces the data thread's process() before it returns, so
	 * n->channels / n->ports are swapped in a clean gap — no RT race), and the
	 * per-channel gain state persists across the rebuild. */
	if (n->stream) {
		pw_stream_destroy(n->stream);
		n->stream = NULL;
	}
	n->position = NULL;   /* the old stream's io area dies with it */
	n->channels = want;
	return sink_open_filter(n, label);
}

const struct reac_box_model *reac_sink_node_recognized_box(const struct reac_sink_node *n)
{
	if (!n)
		return NULL;
	return atomic_load_explicit(&n->pacer.recognized_box, memory_order_acquire);
}

void reac_sink_node_set_peer_source(struct reac_sink_node *n,
                                    struct reac_source_node **src_slot)
{
	if (n)
		n->peer_src = src_slot;
}

/* RT-path trampoline: the pacer announces a new session, the receiver drops what
 * it learned from the last one. Allocation-free and non-blocking by contract. */
static void sink_on_session(void *ctx, const uint8_t mac[6], unsigned session)
{
	if (mac)
		reac_rx_peer_reset((struct reac_rx *)ctx, mac, session);
	else
		reac_rx_session_end((struct reac_rx *)ctx);   /* the session ended */
}

void reac_sink_node_set_rate_source(struct reac_sink_node *n, struct reac_rx *rx)
{
	if (n)
		n->rate_src = rx;
		/* Reset the receiver AT the re-establishment, not a tick later. */
		n->pacer.session_ctx = rx;
		n->pacer.on_session  = sink_on_session;
}

void reac_sink_node_destroy(struct reac_sink_node *n)
{
	if (!n)
		return;
	if (n->log_timer)
		pw_loop_destroy_source(n->loop, n->log_timer);
	if (n->stream)
		pw_stream_destroy(n->stream);   /* stops process() submits first */
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
