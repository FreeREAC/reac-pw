// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_slave.h"
#include "reac_rt.h"
#include <reac/reac_ctrl.h>
#include "reac_mac.h"

#include <reac/reac.h>
#include <reac/reac_encode.h>  /* reac_downstream_build — the MIXER frame (0.5.6) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>          /* powf — used off the RT path (RX handler) only */
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */
#include <sched.h>         /* SCHED_FIFO */
#include <sys/mman.h>      /* mlockall */
#include <signal.h>        /* pthread_sigmask */

/* ------------------------------------------------------------------------- *
 * The PURE decision core — maps the reac_fsm action onto a slave emit kind.
 *
 * reac_fsm is the brain (the gold §13d slave establishment + HOLD). reac_slave
 * adds nothing to the state logic; it only translates the FSM's action into the
 * concrete frame the slave TX path must put on the wire, and threads the FSM's
 * emit_heartbeat flag through. Keeping this a thin, pure mapping means the FSM
 * stays the single source of truth + this layer is trivially offline-testable.
 * ------------------------------------------------------------------------- */

static struct reac_slave_decision map_action(const struct reac_fsm_out *o)
{
	struct reac_slave_decision d = { REAC_SLAVE_EMIT_NONE, 0, 0, o->state };
	switch (o->action) {
	case FSM_ACT_FLOOD_BCAST:
		d.emit = REAC_SLAVE_EMIT_FLOOD_FILLER;   /* bounded broadcast, no join alongside */
		break;
	case FSM_ACT_UNICAST_COLDCONNECT:
		d.emit = REAC_SLAVE_EMIT_COLDCONNECT;
		d.with_join = o->emit_join;   /* FSM gates the cdea 04 03 on the retry grid */
		break;
	case FSM_ACT_UNICAST_AUDIO:
		d.emit = REAC_SLAVE_EMIT_UPSTREAM_AUDIO;
		d.with_heartbeat = o->emit_heartbeat;   /* FSM gates the ~1/s keep-alive */
		break;
	case FSM_ACT_SILENCE:   /* TX-mute window: counter free-runs, nothing emitted */
	case FSM_ACT_STOP:
	case FSM_ACT_NONE:
	default:
		d.emit = REAC_SLAVE_EMIT_NONE;
		break;
	}
	return d;
}

void reac_slave_fsm_init(struct reac_slave *s, const struct reac_slave_cfg *cfg)
{
	memset(s, 0, sizeof *s);
	s->fd = -1;
	reac_fsm_init(&s->fsm);
	s->box_channels = (cfg && cfg->box_channels > 0)
		? (cfg->box_channels > REAC_MAX_CHANNELS ? REAC_MAX_CHANNELS : cfg->box_channels)
		: REAC_SLAVE_BOX_CHANNELS_DEFAULT;
	s->sample_rate = cfg ? cfg->sample_rate : 0;

	/* Head-amp -> input gain: default UNITY on every input until the master sends a
	 * SENS/PAD record, so the upstream audio is byte-identical to today (the memset
	 * already zeroed sens/pad/phantom). Our wire-channel base is the box's fabric
	 * slot offset the console addresses head-amp by: a 16-input S-1608 sits at 0x20,
	 * every other width (S-0808/S-4000S) at 0x00 (m200-headamp-re/DECODE.md). */
	s->ch_base = (s->box_channels == 16) ? 0x20 : 0x00;
	s->box_master = cfg && cfg->box_master;
	s->bm_frame_box = cfg && cfg->box_master_frame_box;
	s->bm_burst_chanmap = cfg && cfg->box_master_burst_chanmap;
	s->bm_fill_noise = cfg && cfg->box_master_fill_noise;
	s->bm_presilence_ms = cfg ? cfg->box_master_presilence_ms : 0;
	s->bm_start_ns = 0;
	snprintf(s->tag, sizeof s->tag, "%s", (cfg && cfg->tag) ? cfg->tag : "");
	/* THE SEND TABLE STARTS EMPTY AND SILENT. An empty reac_headamp_tx is `active`
	 * 0 with no replay armed, so reac_slave_headamp_stamp emits nothing at all until
	 * an operator sets a cell — the wire is byte-identical to before this door
	 * existed until somebody actually asks for 48V. */
	reac_headamp_tx_init(&s->hatx);
	s->bm_rng = 0x1234567u;
	s->bm_chanmap_hit = 0;
	s->bm_announced = 0;
	s->bm_saw_cfea = 0;
	s->bm_last_scene_ns = 0;
	s->bm_burst_sent_ns = 0;
	s->bm_listened = 0;
	s->bm_announce_sent = 0;
	s->bm_burst_just_ended = 0;
	(void)0;   /* the box-master declaration is a captured golden, not a width */
	s->bm_seq = 0;
	s->bm_burst = 0;
	for (int c = 0; c < REAC_MAX_CHANNELS; c++)
		s->ha_gain[c] = 1.0f;

	/* Heartbeat cadence is ~1/s wall-clock, i.e. one keep-alive per frame-rate
	 * worth of frames (fps = rate/12). A real box measured 8162 frames @96k and
	 * 4017 @48k — the gap scales with the rate, so a fixed 8000 would beat at
	 * half-rate on a 48 k desk. Derive it so the same code path fits any rate. */
	if (s->sample_rate > 0)
		s->fsm.heartbeat_period = s->sample_rate / REAC_SAMPLES_PER_PKT;

	/* Default source MAC: the Roland OUI + THIS NIC's host part (never a real
	 * box's — see reac_mac.h). main.c normally passes an already-derived src_mac,
	 * so this is a defensive fallback for a NULL-src cfg (keeps the collision-safe
	 * default in one place instead of a hard-coded box-colliding 0xc4:80:41). */
	if (cfg && cfg->src_mac)
		memcpy(s->src, cfg->src_mac, 6);
	else
		reac_mac_default_src(cfg ? cfg->ifname : NULL, s->src);
}

struct reac_slave_decision reac_slave_step_rx(struct reac_slave *s,
                                              const struct reac_ctrl_parsed *rx)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm, FSM_EV_RX, rx);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

struct reac_slave_decision reac_slave_step_tick(struct reac_slave *s)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm, FSM_EV_TICK, NULL);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

struct reac_slave_decision reac_slave_step_phy(struct reac_slave *s, int up)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm,
	                                      up ? FSM_EV_PHY_UP : FSM_EV_PHY_DOWN, NULL);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

/* ------------------------------------------------------------------------- *
 * The live I/O engine — the shell around the FSM.
 *
 * One AF_PACKET socket, bound to the REAC NIC, does BOTH directions: RX the
 * master's downstream + TX our upstream. The master frame is the clock: each RX
 * of a master frame is one tick, and we emit exactly one upstream frame in
 * response (frame-arrival = our slot clock — the master owns the rate). We do NOT
 * run a clock_nanosleep pacer; locking to the master cadence is the whole point.
 * A short non-blocking idle poll only services the FLOOD re-emit + the TX-mute
 * dwell while no master frame is arriving yet.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- *
 * Received head-amp -> per-input gain (the virtual-stagebox SENS/PAD apply).
 *
 * A real box applies the console's per-channel SENS/PAD to its mic preamp BEFORE
 * the A/D; a virtual box has no preamp, so it must apply the equivalent DIGITAL
 * gain to the audio it returns upstream — else the master's head-amp is inert and
 * the box is useless for testing a session. The model + state update run off the
 * RT path (in the RX handler); the RT upstream path only multiplies.
 * ------------------------------------------------------------------------- */

float reac_slave_headamp_gain(uint8_t sens_value, int pad_on)
{
	/* Input sensitivity S dBu = the level that reaches nominal, so the equivalent
	 * preamp gain is -S dB, with the pad already folded in (pad on -> +20 dBu ->
	 * 20 dB less gain).
	 *
	 * CENTI-dB, though the step measures a whole dB and the integer conversion
	 * would now be exact. Kept because the unit costs nothing and the reason it
	 * was introduced is still live: this ran on whole dB while the codec was a
	 * firmware-derived curve of sub-dB steps, and neighbouring SENS values
	 * collided on one integer, so the virtual box showed the same gain where the
	 * hardware moved. A finer unit cannot produce that failure whatever the curve
	 * turns out to be. */
	int gain_cdb = -reac_headamp_sens_cdb(sens_value, pad_on);
	return powf(10.0f, (float)gain_cdb / 2000.0f);
}

int reac_slave_headamp_rx(struct reac_slave *s, const struct reac_ctrl_parsed *p)
{
	/* Map the WIRE channel (model_base + box_input-1) back to our 0-based input
	 * index; drop records for channels outside our box. */
	int idx = (int)p->ch - s->ch_base;
	if (idx < 0 || idx >= s->box_channels)
		return -1;

	switch (p->param) {
	case REAC_HEADAMP_PHANTOM:
		s->ha_phantom[idx] = p->value ? 1 : 0;
		return idx;                    /* +48V is a voltage, not a gain — state only */
	case REAC_HEADAMP_PAD:
		s->ha_pad[idx] = p->value ? 1 : 0;
		break;
	case REAC_HEADAMP_SENS:
		s->ha_sens[idx] = p->value;
		break;
	default:
		return -1;                     /* unknown param — ignore */
	}

	/* SENS or PAD changed: PRECOMPUTE this input's linear gain here (off the RT
	 * path — never per-sample) and publish it to the staging reader with a relaxed
	 * atomic store. */
	float g = reac_slave_headamp_gain(s->ha_sens[idx], s->ha_pad[idx]);
	atomic_store_explicit(&s->ha_gain[idx], g, memory_order_relaxed);
	return idx;
}

void reac_slave_apply_input_gain(float *const planar[], int nch, int ns,
                                 const _Atomic float *gain)
{
	for (int c = 0; c < nch; c++) {
		float g = atomic_load_explicit(&gain[c], memory_order_relaxed);
		if (g == 1.0f)
			continue;               /* unity — leave the block byte-identical */
		float *b = planar[c];
		for (int i = 0; i < ns; i++)
			b[i] *= g;              /* MULTIPLY-ONLY on the RT path */
	}
}

/* Pull our box_channels of input from the tx_ring into a planar view; on underrun
 * (or no ring) the channels are silent. Returns the per-channel sample count
 * staged (REAC_SAMPLES_PER_PKT or 0). */
static int stage_inputs(struct reac_slave *s,
                        float buf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT],
                        float *planar[REAC_MAX_CHANNELS])
{
	for (int c = 0; c < s->box_channels; c++) {
		planar[c] = buf[c];
		for (int sm = 0; sm < REAC_SAMPLES_PER_PKT; sm++)
			buf[c][sm] = 0.0f;
	}
	if (!s->tx_ring)
		return 0;
	uint32_t got = reac_ring_read_planar(s->tx_ring, planar, (uint32_t)s->box_channels,
	                                      REAC_SAMPLES_PER_PKT);
	if (got == 0)
		return 0;   /* underrun — the zeroed (silent) buffers need no gain */

	/* Apply the received head-amp SENS/PAD as per-input digital gain, exactly as a
	 * real box's preamp would before the A/D. MULTIPLY-ONLY (the gain was precomputed
	 * in the RX handler); all-unity until the master sends head-amp, so this is a
	 * no-op — the upstream stays byte-identical to today until the master drives it. */
	reac_slave_apply_input_gain(planar, s->box_channels, REAC_SAMPLES_PER_PKT, s->ha_gain);
	return REAC_SAMPLES_PER_PKT;
}

/* SAY NOTHING YET (0.5.6-6). The granted S-1608 was silent for about four seconds between
 * losing its old master and beginning its flood, and a box may key its enrolment window on a
 * peer appearing out of silence rather than one that was already talking. Zero by default, so
 * the wire is byte-identical unless the knob is set. The clock starts at the first call, which
 * is the engine's first decision. */
static uint64_t slave_mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* LISTEN BEFORE SPEAKING, for one announce cadence (0.5.6-9).
 *
 * A flood is how a SILENT master is found, and it is noise at one that is calling — but
 * which kind this is cannot be known until it has had a chance to call. A real master
 * announces about once a second, and both boxes that were granted had been quiet far longer
 * than that before they spoke (15 s and 4 s). So the wire stays empty for one cadence, and
 * whether a flood follows is then a fact rather than a guess. `REACPW_BOX_MASTER_PRESILENCE_MS`
 * overrides it in either direction. */
/* TWO ANNOUNCE CADENCES, not one. A master announces about once a second, so a window of
 * one cadence can fall between two of them and the flood starts at a master that was about
 * to call — measured on the veth: 1195 frames went out before the next announce arrived.
 * Two cadences cannot miss a 1 Hz caller, and it is still far short of the 4 s and 15 s the
 * two granted boxes were quiet for. */
#define REAC_BM_LISTEN_MS 2500

static int bm_presilent(struct reac_slave *s)
{
	if (!s->box_master)
		return 0;
	int hold = s->bm_presilence_ms > 0 ? s->bm_presilence_ms : REAC_BM_LISTEN_MS;
	uint64_t now = slave_mono_ns();
	if (!s->bm_start_ns)
		s->bm_start_ns = now;
	if ((now - s->bm_start_ns) < (uint64_t)hold * 1000000ull)
		return 1;
	/* THE BOUND IS COUNTED FROM WHEN WE SPEAK, not from when the engine opened. The FSM
	 * advances its flood counter on every tick whether or not a frame left, so a listening
	 * window silently spends the flood it is there to decide about — measured: 3323 frames
	 * on the wire where the bound is 5460. Reset once, on the way out. */
	if (!s->bm_listened) {
		s->bm_listened = 1;
		/* UNLESS THE MASTER CALLED DURING THE WINDOW, which is the whole point of
		 * listening: the flood was marked satisfied on that frame's arrival, and
		 * resetting the count here would put it straight back. */
		if (!s->bm_saw_cfea)
			s->fsm.flood_frames = 0;
	}
	return 0;
}

/* -60 dBFS OF NOISE IN THE SLOTS, until the grant (0.5.6-6). A hypothesis with a knob: the
 * granted box's flood and pre-grant unicast carried live samples in every slot and ours carry
 * digital silence, because nothing is patched to the sink yet. RT-safe: one multiply-add LCG,
 * no allocation, no syscall. -60 dBFS is far below anything an operator would hear if this
 * ever reached a real output, and it is the pre-grant frames only. */
static void bm_fill(struct reac_slave *s, float *const planar[REAC_MAX_CHANNELS], int nch)
{
	for (int c = 0; c < nch; c++)
		for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++) {
			s->bm_rng = s->bm_rng * 1103515245u + 12345u;
			planar[c][i] += ((float)((int32_t)(s->bm_rng >> 8) & 0xffff) - 32768.0f)
			                / 32768.0f * 0.001f;   /* ~ -60 dBFS */
		}
}

/* HAS THE MASTER'S SCENE TRANSFER STOPPED? (0.5.6-9)
 *
 * Both granted joins landed just after the master's transfer ended — +0.411 s and +0.217 s —
 * and `spec/reac.ksy` says the transfer is repeated until answered and that a box joining
 * mid-transfer must not cancel it. Ours announced on its own clock, inside it, and was
 * refused twice. A master that has never pushed a scene (none was seen) is not waited for:
 * absence of a transfer is not a transfer in progress. */
#define REAC_BM_SCENE_QUIET_NS  (200ull * 1000000ull)   /* the shorter of the two, halved */
#define REAC_BM_RETRY_NS        (2ull * 1000000000ull)  /* no echo in 2 s: ask again */

static int bm_scene_quiet(struct reac_slave *s)
{
	if (!s->bm_last_scene_ns)
		return 1;
	return slave_mono_ns() - s->bm_last_scene_ns >= REAC_BM_SCENE_QUIET_NS;
}

/* THE ESTABLISHED DESCRIPTOR, AND WHEN IT MAY BE CLAIMED (0.5.6-5).
 *
 * `00 7a` sixteen times, filling the control area [18:50] of a FILLER frame. This file
 * already named it — "the descriptor, not the audio, is what marks the ESTABLISHED unicast" —
 * and the rig showed we were saying it too early. Byte for byte against the box that WAS
 * granted (`box-to-box-enroll.pcap`), its unicast fillers carry ZEROS there from the moment
 * it goes unicast (t=6.619) until after the grant lands (6.84), and the descriptor only from
 * 7.619 on. Every frame we sent carried it from the first, which tells a box we are already
 * linked to it before it has granted anything, and a box asked to enrol a peer that claims to
 * be enrolled has nothing left to do.
 *
 * So the claim follows the FSM and nothing else, in either carrier: the descriptor is a
 * statement about the pairing, not about the geometry it rides in. */
static void bm_mark_descriptor(uint8_t *frame, uint8_t d)
{
	for (int i = 0; i < 16; i++) {
		frame[18 + i * 2]     = 0x00;
		frame[18 + i * 2 + 1] = d;
	}
}

/* THE THREE STATES A FILLER'S CONTROL AREA CARRIES (0.5.6-10), read off the wire and
 * confirmed by replay:
 *
 *   zero    before the announce            (the S-0808 sent 48 such frames)
 *   0x52    announce -> grant, "requesting" (8691 frames, exactly its announce-to-burst gap)
 *   0x7a    after the grant, established
 *
 * We sent ZERO for the whole enrolment, and that is the ONLY field-level difference across
 * the two streams: 39992 fillers at zero against their 0x52/0x7a. It is also the one the
 * replay isolated — V9 with its pre-grant descriptor zeroed is REFUSED by the S-1608, where
 * every other element of ours passes inside V9. The S-0808 as master tolerated zeros, which
 * is why this survived the first rig round. */
#define REAC_BM_DESC_REQUESTING 0x52
#define REAC_BM_DESC_ESTABLISHED 0x7a

/* ---- THE MIXER'S SIDE OF A BOX-MASTER WIRE (0.5.6, operator ruling) ---------------
 *
 * "mixer always sends 40ch, boxes send their width only."
 *
 * On a wire a stagebox on M masters we are still the MIXER, so every audio frame we put on
 * it is the ordinary 1492 B, 40-slot downstream — the box's outputs in their slots, the
 * rest silent — in the presence flood and after the grant alike. The 340 B, 8-slot upstream
 * the S-1608 sent that same box is what a BOX sends, and the S-1608 sent it because it IS
 * one. What we take from its capture is the ENROLMENT, not the geometry.
 *
 * So this composes the two: `reac_downstream_build` lays down the mixer's frame (broadcast
 * dst, counter, 40-slot braid, control block left zero — its own contract), and where the
 * enrolment calls for a control frame the 34 bytes at [16:50] are stamped over from the
 * builder that owns those bytes, then re-checksummed. One frame per slot either way, exactly
 * as the box does.
 *
 * ADDRESSING. The audio is broadcast, because a desk's downstream is broadcast on every
 * enrolled wire this daemon has ever driven and the destination address IS the direction on
 * this protocol. The two control frames the enrolment needs — the config-announce and the
 * cold-connect burst — are UNICAST to the box, because that is how the S-1608 sent them to
 * this very chassis and what came back was a grant. */
static size_t bm_downstream(struct reac_slave *s, uint8_t *frame,
                            float *const planar[REAC_MAX_CHANNELS], uint16_t counter)
{
	/* Channels beyond box_channels are silent by the builder's contract, so the box's
	 * outputs land in slots 0..width-1 of the fabric and nothing else is claimed. */
	return (size_t)reac_downstream_build(frame, (float *const *)planar,
	                                     s->box_channels, REAC_SAMPLES_PER_PKT,
	                                     counter, s->src);
}

/* Stamp a control block built by its own builder onto the mixer's frame. `ctl` is a whole
 * frame from one of the reac_ctrl builders; only its 34 bytes at [16:50] — the control
 * marker and the 32-byte block — are taken, and the frame's own checksum is re-applied.
 * Returns 1 when a block was stamped. */
static int bm_stamp(uint8_t *frame, const uint8_t *ctl, size_t ctl_len,
                    const uint8_t dst[6])
{
	if (!ctl_len)
		return 0;
	/* THE DESTINATION IS IN THE FRAME, NOT ONLY IN THE sendto (0.5.6). The protocol's
	 * direction discriminator is the frame's own dst bytes, and that is what a receiver
	 * reads — `reac_downstream_build` writes BROADCAST there by contract, so a control
	 * frame stamped onto one and merely sent to a unicast sockaddr arrives labelled
	 * broadcast. Measured on the veth: three announces and nine cold-connect records
	 * went out with ff:ff:ff:ff:ff:ff in the frame and the emulator counted zero
	 * unicast. The address goes where the reader looks. */
	memcpy(frame, dst, 6);
	memcpy(frame + 16, ctl + 16, 34);
	reac_ctrl_checksum_apply(frame);
	return 1;
}

/* ---- the head-amp SEND door (2026-09-10 ruling; reac_slave.h) ------------- *
 *
 * The same three moving parts the master role has, in the same order and with the
 * same threading rule: a controller ENQUEUES, the engine thread DRAINS into the
 * table (so the table has one writer and the sweep cursor cannot be raced), and the
 * emit path asks the table whether this slot carries a record.
 *
 * WHAT IS NOT DUPLICATED: the scheduler (reac_headamp_tx) and the record builder
 * (reac_ctrl_stamp_headamp) are libreac's, unchanged and role-blind — the whole
 * point of the ruling. Nothing here knows a byte of the protocol. */

int reac_slave_headamp_set(struct reac_slave *s, uint8_t ch, uint8_t param,
                           uint8_t value)
{
	if (!s)
		return 0;
	uint32_t h = atomic_load_explicit(&s->ha_tx_head, memory_order_relaxed);
	uint32_t t = atomic_load_explicit(&s->ha_tx_tail, memory_order_acquire);
	if (h - t >= REAC_SLAVE_HEADAMP_CMD_RING) {
		atomic_fetch_add_explicit(&s->ha_tx_drops, 1, memory_order_relaxed);
		return 0;
	}
	atomic_store_explicit(&s->ha_tx_cmd[h % REAC_SLAVE_HEADAMP_CMD_RING],
	                      reac_headamp_pack(ch, param, value), memory_order_relaxed);
	/* Release after the cell store, so the consumer cannot observe a head index
	 * vouching for a word it has not seen written. */
	atomic_store_explicit(&s->ha_tx_head, h + 1, memory_order_release);
	return 1;
}

int reac_slave_headamp_drain(struct reac_slave *s)
{
	if (!s)
		return 0;
	int applied = 0;
	uint32_t t = atomic_load_explicit(&s->ha_tx_tail, memory_order_relaxed);
	for (;;) {
		uint32_t h = atomic_load_explicit(&s->ha_tx_head, memory_order_acquire);
		if (t == h)
			break;
		uint32_t w = atomic_load_explicit(&s->ha_tx_cmd[t % REAC_SLAVE_HEADAMP_CMD_RING],
		                                  memory_order_relaxed);
		atomic_store_explicit(&s->ha_tx_tail, ++t, memory_order_release);
		uint8_t ch, param, value;
		reac_headamp_unpack(w, &ch, &param, &value);
		/* Validates and silently rejects a bad triple, arms the table, and marks
		 * the cell dirty so the change leaves as an EDGE on the next eligible
		 * slot. Pure array writes: no alloc, no syscall. */
		if (reac_headamp_tx_set(&s->hatx, ch, param, value) == 0 &&
		    ch < REAC_HEADAMP_MAX_CH && param < REAC_HEADAMP_NPARAMS) {
			/* SET, not incremented: this absolute value replaces whatever that
			 * cell was going to say, so its remaining sends are the new value's,
			 * not the old one's plus the new one's. */
			s->ha_rep[ch][param] = REAC_SLAVE_HEADAMP_EDGE_SENDS;
			s->ha_rep_wait = 0;    /* the FIRST send is not delayed */
		}
		applied++;
	}
	if (applied)
		atomic_fetch_add_explicit(&s->ha_tx_applied, (uint64_t)applied,
		                          memory_order_relaxed);
	return applied;
}

int reac_slave_headamp_stamp(struct reac_slave *s, uint8_t *frame,
                             int slot_has_ctrl, int linked)
{
	if (!s || !frame || slot_has_ctrl || !linked)
		return 0;
	/* NOT BEFORE THE PAIRING IS REAL, and never over another control block. The
	 * enrolment's own frames — the declaration, the cold-connect burst, the
	 * heartbeat — are what the peer answers, and a preamp record written over one
	 * of them would be a lost grant, not a lost knob. Same guard the master role
	 * states as "only ever a FILLER slot, and only once ESTABLISHED". */
	if (!s->hatx.active && !s->hatx.replay_width)
		return 0;
	uint8_t ch, param, value;
	if (!reac_headamp_tx_next(&s->hatx, &ch, &param, &value)) {
		/* NOTHING NEW — BUT A SEND MAY STILL BE OWED. The table emits one record
		 * per changed cell and then goes quiet; with no readback and no scene
		 * replay on this segment, that would make one lost frame one lost setting.
		 * Re-dirty the cell that still owes sends, spaced by the protocol's own
		 * record stride, and let the SAME scheduler and the SAME builder emit it. */
		if (s->ha_rep_wait > 0) {
			s->ha_rep_wait--;
			return 0;
		}
		int rc = -1, rp = -1;
		for (int c = 0; c < REAC_HEADAMP_MAX_CH && rc < 0; c++)
			for (int q = 0; q < REAC_HEADAMP_NPARAMS; q++)
				if (s->ha_rep[c][q]) { rc = c; rp = q; break; }
		if (rc < 0)
			return 0;
		if (reac_headamp_tx_set(&s->hatx, (uint8_t)rc, (uint8_t)rp,
		                        s->hatx.value[rc][rp]) != 0 ||
		    !reac_headamp_tx_next(&s->hatx, &ch, &param, &value))
			return 0;
	}
	if (reac_ctrl_stamp_headamp(frame, ch, param, value) != 0)
		return 0;
	if (ch < REAC_HEADAMP_MAX_CH && param < REAC_HEADAMP_NPARAMS &&
	    s->ha_rep[ch][param]) {
		s->ha_rep[ch][param]--;
		s->ha_rep_wait = REAC_HEADAMP_SWEEP_STRIDE;
	}
	/* COUNT WHAT WENT ON THE WIRE, not what was accepted at the door. A PATCH that
	 * is queued, a cell that is set and a record that is stamped are three different
	 * facts, and only this one is evidence that anything was asked of the box. */
	atomic_fetch_add_explicit(&s->ha_tx_records, 1, memory_order_relaxed);
	return 1;
}

/* Emit one frame for the decision `d` on the wire. `bcast` = the broadcast dst
 * (presence-flood), else the learned master MAC (unicast linked traffic). */
static void emit_decision(struct reac_slave *s, const struct reac_slave_decision *d,
                          struct sockaddr_ll *bcast_sll, struct sockaddr_ll *uni_sll)
{
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	uint8_t frame[2048];
	size_t len = 0;
	struct sockaddr_ll *sll = uni_sll;
	uint16_t counter = s->fsm.counter;

	float buf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];

	/* EXACTLY ONE frame per call — one wire frame per monotonic counter value, as a
	 * real S-1608 (#130, byte-verified 2026-07-11). Control frames REPLACE the
	 * audio/flood frame at this slot; they never add a second frame. */
	/* THE MIXER'S GEOMETRY, THE BOX'S CHOREOGRAPHY (0.5.6, operator ruling). Taken
	 * before the switch because every arm of it would otherwise have to remember. The
	 * FSM, its bounds and its transitions are untouched: only what a slot puts on the
	 * wire changes, and only on a wire a stagebox masters. */
	if (s->box_master && d->emit != REAC_SLAVE_EMIT_NONE) {
		uint8_t ctl[2048] = { 0 };
		size_t cl = 0;
		int flooding = (d->emit == REAC_SLAVE_EMIT_FLOOD_FILLER);
		stage_inputs(s, buf, planar);
		/* NOT LINKED UNTIL THE GRANT IS ACCEPTED. The FSM is the only thing that
		 * knows, and it is what the descriptor below follows. */
		int linked = (s->fsm.state == FSM_ESTABLISHED);
		if (bm_presilent(s))
			return;                 /* the wire stays empty for the opening window */
		if (!linked && s->bm_fill_noise)
			bm_fill(s, planar, s->box_channels);
		if (s->bm_frame_box) {
			/* THE EXPERIMENT'S OTHER CORNER (0.5.6-3): an exact S-1608 imitation.
			 * The frame is the box's own 340 B geometry at the master's width, in
			 * the flood, as the carrier of the announce and the burst, and in the
			 * steady state — and everything after the flood is unicast to the box,
			 * which is how the box that WAS granted sent it.
			 *
			 * BEFORE THE GRANT THE CARRIER IS THE FLOOD'S, JUST UNICAST (0.5.6-5):
			 * that builder leaves the control area ZERO, which is what the granted
			 * box sent for the whole cold-connect. The upstream filler — the one
			 * that stamps the ESTABLISHED descriptor — is only reached once we are
			 * actually established. */
			if (flooding)
				len = reac_ctrl_build_flood_filler(frame, BCAST, s->src, counter,
				          s->box_channels, planar, REAC_SAMPLES_PER_PKT);
			else if (linked)
				len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac,
				          s->src, counter, s->box_channels, planar,
				          REAC_SAMPLES_PER_PKT);
			else {
				len = reac_ctrl_build_flood_filler(frame, s->fsm.master_mac,
				          s->src, counter, s->box_channels, planar,
				          REAC_SAMPLES_PER_PKT);
				/* zero until we have asked, then REQUESTING until granted */
				if (len && s->bm_announce_sent)
					bm_mark_descriptor(frame, REAC_BM_DESC_REQUESTING);
			}
			sll = flooding ? bcast_sll : uni_sll;
		} else {
			/* `reac_downstream_build` leaves the control area zero by contract, so
			 * the mixer carrier is already honest before the grant; the claim is
			 * ADDED once the pairing is real. Same state, same field, either
			 * geometry. */
			len = bm_downstream(s, frame, planar, counter);
			if (len && linked)
				bm_mark_descriptor(frame, REAC_BM_DESC_ESTABLISHED);
			else if (len && s->bm_announce_sent)
				bm_mark_descriptor(frame, REAC_BM_DESC_REQUESTING);
			sll = bcast_sll;    /* a desk's downstream is broadcast */
		}

		if (d->emit == REAC_SLAVE_EMIT_COLDCONNECT && s->bm_burst_just_ended) {
			/* THE FRAME AFTER THE BURST IS A HEARTBEAT, and both granted boxes send
			 * it BEFORE the grant, not after establishment:
			 *
			 *   16.1162  cdea 0403 0014      the JOIN
			 *   16.1164  cdea 0403 0013      BOX_READY
			 *   16.1165  cdea 0103 0001 81   one frame later, 2 ms before the grant
			 *
			 * Ours heartbeat only once ESTABLISHED, so the master was being asked to
			 * grant a peer that had not said it was there. Every FIELD of our control
			 * frames is byte-identical to a granted one — this is a frame that is
			 * MISSING, which is why nine rounds of byte diffs did not see it. It has
			 * to be taken before the cold-connect grid below, which is still running
			 * and would otherwise own every slot. */
			s->bm_burst_just_ended = 0;
			cl = reac_ctrl_build_box_hb(ctl, s->fsm.master_mac, s->src, counter,
			                            s->box_channels);
		} else if (d->emit == REAC_SLAVE_EMIT_COLDCONNECT) {
			/* The two control frames the enrolment needs, in the order the wire
			 * showed and unicast as the wire showed: the config-announce as the
			 * frame we go unicast with, the three-record burst ~200 ms later. */
			if (s->bm_burst > 0) {
				/* THREE DISTINCT RECORDS — tags 0100, 0000, 0302, which is what
				 * `spec/reac.ksy` states the burst is and what libreac builds
				 * (`reac_ctrl_build_coldconnect{,_head,_0013}`). This
				 * arm used to call the 0014 builder for both of the first two
				 * frames, so our burst was one record twice — and the box
				 * answers one echo per DISTINCT record, so it was being asked
				 * for a two-record answer where a real box asks for three. */
				if (s->bm_burst == 3)
					cl = reac_ctrl_build_coldconnect(ctl, s->fsm.master_mac,
					         s->src, counter, s->box_channels, planar,
					         REAC_SAMPLES_PER_PKT);
				else if (s->bm_burst == 2)
					cl = reac_ctrl_build_coldconnect_head(ctl,
					         s->fsm.master_mac, s->src, counter,
					         s->box_channels, planar, REAC_SAMPLES_PER_PKT);
				else {
					cl = reac_ctrl_build_coldconnect_0013(ctl, s->fsm.master_mac,
					         s->src, counter, s->box_channels, planar,
					         REAC_SAMPLES_PER_PKT);
					/* the heartbeat rides the very next slot (see above) */
					s->bm_burst_just_ended = 1;
				}
				s->bm_burst--;
			} else if (d->with_join) {
				if (s->bm_burst_chanmap) {
					/* ARMED BY THE BOX, NOT BY OUR OWN GRID. The announce goes
					 * out once and then the burst waits for the box's chanmap —
					 * the frame the granted box's burst landed one millisecond
					 * behind. Re-announce only if the wait is not answered
					 * within a full grid cycle, so a box that never chanmaps
					 * cannot leave us silent for ever. */
					if (!s->bm_announced) {
						cl = reac_ctrl_build_config_announce_box_master(
						         ctl, s->fsm.master_mac, s->src,
						         counter, s->box_channels);
						s->bm_announce_sent = 1;
						s->bm_announced = 1;
						s->bm_chanmap_hit = 0;
					} else if (s->bm_chanmap_hit) {
						s->bm_chanmap_hit = 0;
						s->bm_burst = 3;
					}
					if (s->bm_burst == 3) {
						cl = reac_ctrl_build_coldconnect(ctl,
						         s->fsm.master_mac, s->src, counter,
						         s->box_channels, planar,
						         REAC_SAMPLES_PER_PKT);
						s->bm_burst--;
					}
					if (++s->bm_seq >= 24)
						{ s->bm_seq = 0; s->bm_announced = 0; }
				} else if (s->bm_seq == 0 && bm_scene_quiet(s)) {
					/* WE DECLARE OURSELVES, NOT THE PEER — and we declare the
					 * one thing this chassis has ever granted. 0.5.6-1 derived
					 * the declaration from the MASTER's width and announced
					 * selector 0x84, the family of the box it was talking TO;
					 * the rig sent four correct bursts behind it and was echoed
					 * nothing, lamp blinking. The captured block above is the
					 * declaration that was granted. */
					/* OUR OWN INVENTORY, at the width we send. Announcing
					 * the S-1608's table to an S-1608 master told it its own
					 * identity and was refused twice; the S-0808 that WAS
					 * granted by that master declared its own 8-input table.
					 * `box_channels` here is the master's output count, which
					 * is the number of slots we fill (0.5.6-9). */
					cl = reac_ctrl_build_config_announce_box_master(
					         ctl, s->fsm.master_mac, s->src, counter,
					         s->box_channels);
					s->bm_announce_sent = 1;
				}
				else if (s->bm_seq == 2)
					s->bm_burst = 3;
				if (s->bm_burst == 3) {
					cl = reac_ctrl_build_coldconnect(ctl, s->fsm.master_mac,
					         s->src, counter, s->box_channels, planar,
					         REAC_SAMPLES_PER_PKT);
					s->bm_burst--;
				}
				/* THE BURST IS SENT ONCE AND THEN WE WAIT (0.5.6-9). Every
				 * granted sequence on this rig was announce -> burst -> echo
				 * within milliseconds; ours retried the pair every 0.8 s and
				 * never sat still long enough to be answered. Retry only if no
				 * echo has arrived within the bound, and never re-flood: the
				 * master is known, and hunting one that is already answering is
				 * how a retry becomes noise. */
				if (s->bm_burst == 0 && s->bm_seq >= 3 && !s->bm_burst_sent_ns)
					s->bm_burst_sent_ns = slave_mono_ns();
				if (s->bm_burst_sent_ns) {
					if (slave_mono_ns() - s->bm_burst_sent_ns
					      >= REAC_BM_RETRY_NS) {
						s->bm_burst_sent_ns = 0;
						s->bm_seq = 0;
					}
				} else if (++s->bm_seq >= 8) {
					s->bm_seq = 0;
				}
			}
		} else if (d->emit == REAC_SLAVE_EMIT_HEARTBEAT || d->with_heartbeat) {
			/* THE HEARTBEAT RIDES TWO DECISIONS, and missing the second one is a
			 * segment that establishes and then says nothing: the FSM carries it
			 * as its own emit AND as a flag on the established audio slot
			 * (reac_slave.h), and the desk path handles both. Measured on the
			 * veth: established, and not one keep-alive reached the box. */
			cl = reac_ctrl_build_box_hb(ctl, s->fsm.master_mac, s->src, counter,
			                            s->box_channels);
		}
		/* A MIXER DOES NOT GO SILENT MID-DWELL. The FSM's TX-mute window is a box
		 * holding ITS upstream back while the master settles; holding a desk's
		 * downstream back would take the box's outputs away for the length of it.
		 * The slot still carries the ordinary broadcast frame. */
		int stamped_ctl = cl && bm_stamp(frame, ctl, cl, s->fsm.master_mac);
		if (stamped_ctl)
			sll = uni_sll;      /* the control frames are the box's to answer */
		/* THE OPERATOR'S 48V SWITCH, ON A WIRE THE BOX MASTERS (2026-09-10 ruling).
		 * Guarded exactly as the master role guards its own overlay: never over a
		 * control block this slot already carries, and never before ESTABLISHED. It
		 * DOES overwrite the frame's descriptor word for this one slot, which is
		 * what a real M-200 does too — the descriptor is repeated 8000 times a
		 * second and the record is a handful of frames.
		 *
		 * ADDRESSED AS THE RIG ADDRESSED IT: `sll` is left alone, so the record
		 * rides the broadcast mixer carrier. That is what reac-pw put on enp131s0 at
		 * this very box on 2026-09-09 (ha-write-s1608.pcap: dst ff:ff:ff:ff:ff:ff,
		 * 1492 B) and what a real console does — a head-amp command is broadcast on
		 * the segment, not unicast to a chassis. */
		reac_slave_headamp_stamp(s, frame, stamped_ctl, linked);
		if (len) {
			ssize_t r = sendto(s->fd, frame, len, 0,
			                   (struct sockaddr *)sll, sizeof *sll);
			if (r < 0)
				atomic_fetch_add_explicit(&s->tx_errors, 1, memory_order_relaxed);
			else
				atomic_fetch_add_explicit(&s->tx_frames, 1, memory_order_relaxed);
		}
		return;
	}

	switch (d->emit) {
	case REAC_SLAVE_EMIT_NONE:
		return;

	case REAC_SLAVE_EMIT_FLOOD_FILLER:
		/* §13p.3: announce by FLOODING broadcast FILLER — BOUNDED (~1.36 s), the
		 * FSM caps it and then switches to the unicast cold-connect phase. The dst
		 * is broadcast; the master is being learned from its L2 source on RX. Zero
		 * control block [18:50] (no 0x7a descriptor) over LIVE staged input audio
		 * [50:626]: on a real box the flood's audio region varies every frame — the
		 * descriptor, not the audio, is what marks the ESTABLISHED unicast. */
		stage_inputs(s, buf, planar);
		len = reac_ctrl_build_flood_filler(frame, BCAST, s->src, counter,
		                                   s->box_channels, planar,
		                                   REAC_SAMPLES_PER_PKT);
		sll = bcast_sll;
		break;

	case REAC_SLAVE_EMIT_COLDCONNECT:
		/* Flood done, master learned: unicast-only. On the ~100 ms retry grid this
		 * slot carries the cold-connect (cdea 04 03, the box's JOIN trigger — the
		 * master echoes its block back as the grant); otherwise a unicast audio
		 * FILLER. One frame either way, always to the learned master. */
		stage_inputs(s, buf, planar);
		if (s->box_master) {
			/* THE ORDER A REAL BOX USES TOWARDS A BOX ON M (0.5.6, ground truth in
			 * DESIGN.md). The S-1608 went unicast at t=6.619 s WITH its
			 * config-announce as that very frame, and sent the cold-connect as
			 * three CONSECUTIVE frames 214 ms later — not one record per grid slot.
			 * So the grid carries the announce at slot 0 and arms a 3-frame burst
			 * at slot 2; everything between is unicast FILLER, which is what the
			 * capture shows too. The pair repeats every 8 slots until the grant, in
			 * place of the desk escalation, because a peer that never answers must
			 * be re-asked and there is no other record it has ever been seen to
			 * want. */
			if (s->bm_burst > 0) {
				len = (s->bm_burst == 3 || s->bm_burst == 2)
					? reac_ctrl_build_coldconnect(frame, s->fsm.master_mac,
					      s->src, counter, s->box_channels, planar,
					      REAC_SAMPLES_PER_PKT)
					: reac_ctrl_build_coldconnect_0013(frame, s->fsm.master_mac,
					      s->src, counter, s->box_channels, planar,
					      REAC_SAMPLES_PER_PKT);
				s->bm_burst--;
			} else if (d->with_join) {
				if (s->bm_seq == 0)
					len = reac_ctrl_build_config_announce(frame,
					          s->fsm.master_mac, s->src, counter,
					          s->box_channels);
				else if (s->bm_seq == 2)
					s->bm_burst = 3;   /* fires on this frame and the next two */
				if (s->bm_burst == 3) {
					len = reac_ctrl_build_coldconnect(frame, s->fsm.master_mac,
					          s->src, counter, s->box_channels, planar,
					          REAC_SAMPLES_PER_PKT);
					s->bm_burst--;
				}
				if (++s->bm_seq >= 8)
					s->bm_seq = 0;
			}
			if (len == 0)
				len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac,
				          s->src, counter, s->box_channels, planar,
				          REAC_SAMPLES_PER_PKT);
		} else if (d->with_join) {
			/* Escalate through the FULL cold-connect sequence a real S-1608 sends —
			 * 0014 -> 0013 -> 0016 -> 001a (byte-matched to m5000-s1608 establish,
			 * 2026-07-11). The 0016/001a carry the fuller box inventory the master
			 * needs to register the box; emitting only 0014/0013 left the desk blind
			 * (live M-5000 test, 2026-07-11). One variant per join grid slot. */
			switch (s->coldconnect_phase % 8) {
			case 1:
				len = reac_ctrl_build_coldconnect_0013(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels, planar, REAC_SAMPLES_PER_PKT);
				break;
			case 2:
				len = reac_ctrl_build_coldconnect_0016(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels, planar, REAC_SAMPLES_PER_PKT);
				break;
			case 3:
				len = reac_ctrl_build_coldconnect_001a(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels, planar, REAC_SAMPLES_PER_PKT);
				break;
			case 4:
				/* ANNOUNCE OUR SETUP — the master enrolls the box from this frame;
				 * without it the desk never registers us (live M-5000 test). */
				len = reac_ctrl_build_config_announce(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels);
				break;
			case 5:
				/* the box also heartbeats DURING cold-connect, before any grant */
				len = reac_ctrl_build_box_hb(frame, s->fsm.master_mac, s->src, counter,
				          s->box_channels);
				break;
			case 6:
				/* ANNOUNCE OUR EXACT MODEL — the FIRST half of the identity record:
				 * the DT1 preamble, TAG 0x0500 and the ASCII model name. Required for
				 * the 0x84 family (S-0808 etc.) so the desk shows the real model, not
				 * the generic family name (live M-200, 2026-07-11). Returns 0 for the
				 * 0x82 family (named by selector) -> emit a plain upstream filler
				 * instead. Case 7 carries the other half and MUST follow it. */
				len = reac_ctrl_build_identity_first(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels);
				if (len == 0)
					len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac,
					          s->src, counter, s->box_channels, planar,
					          REAC_SAMPLES_PER_PKT);
				break;
			case 7:
				/* THE OTHER HALF OF THE SAME MESSAGE. The identity record is one
				 * Roland SysEx that arrives as two link-4 fragments, and its inner
				 * checksum closes only ACROSS BOTH: this one carries that closing
				 * byte and the f7 that ends it. Sending case 6 without this puts a
				 * record on the wire nothing can verify, which is why one flag in
				 * the model row gates both and both return 0 for the same models.
				 * It rides the very next grid slot, so the pair stays in order. */
				len = reac_ctrl_build_identity_last(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels);
				if (len == 0)
					len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac,
					          s->src, counter, s->box_channels, planar,
					          REAC_SAMPLES_PER_PKT);
				break;
			default:
				len = reac_ctrl_build_coldconnect(frame, s->fsm.master_mac, s->src,
				          counter, s->box_channels, planar, REAC_SAMPLES_PER_PKT);
				break;
			}
			s->coldconnect_phase++;
		} else {
			len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac, s->src, counter,
			                                      s->box_channels, planar,
			                                      REAC_SAMPLES_PER_PKT);
		}
		break;

	case REAC_SLAVE_EMIT_UPSTREAM_AUDIO:
		/* Established: unicast our input channels upstream at the box's slots — a
		 * slave sends its inputs INTO the REAC stream. The ~1/s keep-alive REPLACES
		 * the audio frame on the slot the FSM flags it (a box's sparse heartbeat
		 * occupies an audio slot, never an extra frame). */
		stage_inputs(s, buf, planar);
		if (d->with_heartbeat)
			len = reac_ctrl_build_box_hb(frame, s->fsm.master_mac, s->src, counter, s->box_channels);
		else
			len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac, s->src, counter,
			                                      s->box_channels, planar,
			                                      REAC_SAMPLES_PER_PKT);
		break;

	case REAC_SLAVE_EMIT_HEARTBEAT:
		len = reac_ctrl_build_box_hb(frame, s->fsm.master_mac, s->src, counter, s->box_channels);
		break;
	}

	if (len == 0)
		return;

	ssize_t r = sendto(s->fd, frame, len, 0, (struct sockaddr *)sll, sizeof *sll);
	if (r < 0)
		atomic_fetch_add_explicit(&s->tx_errors, 1, memory_order_relaxed);
	else
		atomic_fetch_add_explicit(&s->tx_frames, 1, memory_order_relaxed);
}

static void *slave_loop(void *arg)
{
	struct reac_slave *s = arg;

	/* RT hardening — the reac_pacer.c / reac_repacer.c recipe that streams to real
	 * stageboxes flawlessly AND re-synced our boxes to an M-5000 over WiFi: lock
	 * memory, go SCHED_FIFO, block signals, so our upstream emission has low,
	 * consistent latency. A real box's PLL-clocked upstream is jitter-free and the
	 * master locks its word clock to it before granting. Best-effort — without
	 * CAP_SYS_NICE/rtprio we run SCHED_OTHER (jittery, may not link).
	 *
	 * The priority is the wire-clock band of reac_rt.h, resolved in
	 * reac_slave_open: an upstream engine that outranks the PipeWire graph
	 * preempts the very cycle that fills the ring it sends from. */
	mlockall(MCL_CURRENT | MCL_FUTURE);
	reac_rt_thread_go("reac_slave", s->prio, s->prio_src);
	sigset_t allsig; sigfillset(&allsig); pthread_sigmask(SIG_BLOCK, &allsig, NULL);

	struct sockaddr_ll bcast_sll, uni_sll;
	memset(&bcast_sll, 0, sizeof bcast_sll);
	bcast_sll.sll_family = AF_PACKET;
	bcast_sll.sll_ifindex = s->ifindex;
	bcast_sll.sll_halen = 6;
	memset(bcast_sll.sll_addr, 0xFF, 6);
	uni_sll = bcast_sll;   /* sll_addr patched to the master MAC once learned */

	uint8_t rxbuf[2048];

	static const char *const st_name[] = {
		"PHY_DOWN", "FLOOD_ANNOUNCE", "COLDCONNECT", "TX_MUTE", "ESTABLISHED", "DROP"
	};
	enum reac_fsm_state prev_state = s->fsm.state;
	fprintf(stderr, "reac_slave: %sSTATE %s\n", s->tag, st_name[prev_state]);

	while (atomic_load_explicit(&s->running, memory_order_acquire)) {
		/* State-transition trace (task #130): the FSM's phase is the ground truth
		 * for establishment — log every change so a live run shows FLOOD ->
		 * COLDCONNECT -> (grant) TX_MUTE -> ESTABLISHED and any DROP/re-flood flap. */
		if (s->fsm.state != prev_state) {
			fprintf(stderr, "reac_slave: %sSTATE %s -> %s%s\n", s->tag,
			        st_name[prev_state], st_name[s->fsm.state],
			        s->fsm.state == FSM_DROP ? " (drop)" : "");
			prev_state = s->fsm.state;
		}
		/* Absorb any head-amp change a controller pushed since the last slot, on
		 * THIS thread so the table stays single-writer. Every tick regardless of
		 * state, so a change is already in the table the instant an ESTABLISHED slot
		 * comes round; two relaxed loads when the ring is empty. */
		reac_slave_headamp_drain(s);
		/* Apply a pending PHY change on THIS thread (the FSM owner). */
		int want = atomic_load_explicit(&s->phy_up_req, memory_order_acquire);
		if (want != s->phy_up_seen) {
			struct reac_slave_decision d = reac_slave_step_phy(s, want);
			s->phy_up_seen = want;
			if (want) {             /* PHY up: begin the presence-flood immediately */
				s->counter_locked = 0;   /* re-latch the master offset on a fresh link */
				emit_decision(s, &d, &bcast_sll, &uni_sll);
			}
		}

		/* Block (with a short timeout) for the next master frame. Frame-arrival is
		 * the clock tick we lock to; a timeout services the flood/dwell self-clock. */
		ssize_t n = recv(s->fd, rxbuf, sizeof rxbuf, 0);
		if (n <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* No master frame this window — self-clock the FSM one tick so the
				 * FLOOD re-emit + TX-mute dwell + peer-gone countdown still advance. */
				struct reac_slave_decision d = reac_slave_step_tick(s);
				memcpy(uni_sll.sll_addr, s->fsm.master_mac, 6);
				emit_decision(s, &d, &bcast_sll, &uni_sll);
				continue;
			}
			if (errno == EINTR)
				continue;
			break;
		}
		if (!reac_frame_is_reac(rxbuf, (size_t)n))
			continue;

		struct reac_ctrl_parsed p;
		reac_ctrl_parse(rxbuf, (size_t)n, &p);

		/* Surface a received head-amp record (task B.4 / #33): the master (a real
		 * console) broadcasts preamp commands as op 04 03 TAG 01 01 — proving we
		 * classify a knob-turn as HEADAMP, not as our JOIN grant. Purely
		 * observational: HEADAMP is master EVIDENCE in the FSM but never a grant
		 * (reac_fsm.c is_master_frame), so logging it does NOT touch establishment.
		 * Verify the inner record checksum first — a corrupted record must not be
		 * reported as a real preamp value. */
		if (p.kind == REAC_CTRL_HEADAMP) {
			if (reac_ctrl_headamp_record_verify(rxbuf) != 0) {
				fprintf(stderr, "reac_slave: head-amp record (ch %u param %u) "
				        "DROPPED — inner checksum bad\n", p.ch, p.param);
			} else {
				/* APPLY it as our INPUT GAIN (task #202): a virtual box has no
				 * preamp, so the console's SENS/PAD must scale the audio we send
				 * upstream, or the master's head-amp is inert. Updates per-input
				 * state + precomputes the linear gain off the RT path; returns our
				 * 0-based input index, or -1 for a channel outside our box. */
				int idx = reac_slave_headamp_rx(s, &p);
				if (idx < 0) {
					fprintf(stderr, "reac_slave: head-amp RX ch %u %s — not one of "
					        "our %d inputs (base 0x%02x), ignored\n", p.ch,
					        reac_headamp_param_name(p.param), s->box_channels,
					        s->ch_base);
				} else if (p.param == REAC_HEADAMP_SENS) {
					fprintf(stderr, "reac_slave: head-amp RX ch %u (input %d) %s "
					        "value 0x%02x (%d dB, pad %s) -> gain x%.3f\n", p.ch, idx,
					        reac_headamp_param_name(p.param), p.value,
					        reac_headamp_sens_db(p.value, s->ha_pad[idx]),
					        s->ha_pad[idx] ? "on" : "off",
					        atomic_load_explicit(&s->ha_gain[idx],
					                             memory_order_relaxed));
				} else {
					fprintf(stderr, "reac_slave: head-amp RX ch %u (input %d) %s %s\n",
					        p.ch, idx, reac_headamp_param_name(p.param),
					        p.value ? "on" : "off");
				}
			}
		}

		/* Only frames FROM a master are the clock + drive establishment; ignore any
		 * frame whose source is us (loopback) or another box. The FSM learns the
		 * master MAC from the L2 source of a master-kind frame. */
		if (memcmp(p.src, s->src, 6) == 0)
			continue;   /* our own echo on a hub/loopback — not a tick */

		/* THE BOX'S CHANMAP, when the burst is armed by it (0.5.6-5). Same thread as
		 * the emit below, so a plain flag is the whole synchronisation. */
		if (s->bm_burst_chanmap && p.kind == REAC_CTRL_MASTER_HB)
			s->bm_chanmap_hit = 1;

		if (s->box_master) {
			/* A MASTER THAT ANNOUNCES ITSELF NEEDS NO HUNTING (0.5.6-9). The
			 * S-1608 in master mode sends `cfea` about once a second and the
			 * S-0808 sends none — and the box that joined the announcing one
			 * broadcast NOTHING, while the box that joined the silent one flooded
			 * for 0.68 s first. So the flood is how a silent master is FOUND, and
			 * it is noise at one that is calling. The FSM's own bound is what ends
			 * it; telling it the bound is reached is the engine saying the flood's
			 * purpose is already served. */
			if (p.kind == REAC_CTRL_MASTER_ANNOUNCE && !s->bm_saw_cfea) {
				s->bm_saw_cfea = 1;
				if (s->fsm.state == FSM_FLOOD_ANNOUNCE) {
					s->fsm.flood_frames = REAC_FSM_FLOOD_BURST;
					fprintf(stderr, "reac_slave: %sthe master announces itself "
					        "— no flood needed, going straight to the "
					        "cold-connect\n", s->tag);
				}
			}
			/* AND A JOIN LANDS AFTER ITS SCENE TRANSFER STOPS. Measured twice:
			 * +0.411 s and +0.217 s after the last scene record, and the ksy says
			 * a box joining mid-transfer must not cancel it. */
			if (p.kind == REAC_CTRL_SCENE_TRANSFER)
				s->bm_last_scene_ns = slave_mono_ns();
		}

		atomic_fetch_add_explicit(&s->rx_master_frames, 1, memory_order_relaxed);

		struct reac_slave_decision d = reac_slave_step_rx(s, &p);
		memcpy(uni_sll.sll_addr, s->fsm.master_mac, 6);  /* learned this step */

		/* Publish the learned master to the main loop's segment answer, as one
		 * atomic (reac_slave.h). Stored only when it MOVES, so the established
		 * steady state costs a compare and not a store on every one of the
		 * 8000 frames a second this loop runs at. */
		if (s->fsm.have_master) {
			uint64_t mac48 = reac_mac48_pack(s->fsm.master_mac);
			if (mac48 != atomic_load_explicit(&s->master_mac48, memory_order_relaxed))
				atomic_store_explicit(&s->master_mac48, mac48, memory_order_relaxed);
		}

		/* Follow the master clock (the M-200i is the word-clock master): override
		 * the FSM's free-running counter with one LOCKED to the master's downstream
		 * counter at a fixed offset, latched at first lock. A real box's upstream
		 * counter tracks the master's exactly (constant offset, one frame per
		 * master frame); a box whose counter free-runs from zero / drifts is not
		 * clock-slaved and the master refuses it. State transitions don't use the
		 * counter, so overriding it here (right before emit) is safe. */
		if (s->fsm.have_master && memcmp(p.src, s->fsm.master_mac, 6) == 0) {
			if (!s->counter_locked) {
				s->counter_offset = (uint16_t)(s->fsm.counter - p.counter);
				s->counter_locked = 1;
			}
			s->fsm.counter = (uint16_t)(p.counter + s->counter_offset);
		}

		emit_decision(s, &d, &bcast_sll, &uni_sll);
	}
	return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

int reac_slave_open(struct reac_slave *s, const struct reac_slave_cfg *cfg,
                    struct reac_ring *tx_ring)
{
	reac_slave_fsm_init(s, cfg);
	s->tx_ring = tx_ring;
	/* Resolved HERE, on the caller's thread: reac_rt_prio_resolve reads the
	 * layered config files, which the engine thread must never do. */
	s->prio = reac_rt_prio_resolve(cfg->prio, NULL, &s->prio_src);

	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, cfg->ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	s->ifindex = ifr.ifr_ifindex;

	/* Bind to the interface so recv() yields only this NIC's 0x8819 frames. */
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(REAC_ETHERTYPE);
	sll.sll_ifindex = s->ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		close(fd);
		return -1;
	}

	/* Short RX timeout so recv() returns periodically to self-clock the flood/dwell
	 * even before any master frame arrives (~5 ms — well under the 600-frame HOLD
	 * budget at every rate, and the master frame, when present, clocks us instead). */
	struct timeval tv = { 0, 5000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	/* PROMISCUOUS WHEN OUR SOURCE IS NOT THIS NIC'S OWN (0.5.6). A box unicasts to the
	 * address it was announced from, and the card's hardware filter drops a unicast to an
	 * address it does not own — the same reason reac_pacer takes PACKET_MR_PROMISC for the
	 * master role, which spoofs nothing but receives a box's unicast to a learned MAC.
	 * Without this, a Roland-OUI stand-in would trade a box that will not grant for a box
	 * whose grant we cannot hear. Best-effort: reported, never fatal. */
	if (s->box_master) {
		struct packet_mreq mr;
		memset(&mr, 0, sizeof mr);
		mr.mr_ifindex = ifr.ifr_ifindex;
		mr.mr_type    = PACKET_MR_PROMISC;
		if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr) < 0)
			fprintf(stderr, "reac_slave: PACKET_MR_PROMISC failed — a box's unicast to "
			                "our announced address may not reach us\n");
	}

	s->fd = fd;
	return 0;
}

int reac_slave_start(struct reac_slave *s)
{
	atomic_store_explicit(&s->running, 1, memory_order_release);
	if (pthread_create(&s->thread, NULL, slave_loop, s) != 0) {
		atomic_store_explicit(&s->running, 0, memory_order_release);
		return -1;
	}
	return 0;
}

void reac_slave_set_phy_up(struct reac_slave *s, int up)
{
	atomic_store_explicit(&s->phy_up_req, up ? 1 : 0, memory_order_release);
}

void reac_slave_stop(struct reac_slave *s)
{
	if (!atomic_load_explicit(&s->running, memory_order_acquire))
		return;
	atomic_store_explicit(&s->running, 0, memory_order_release);
	pthread_join(s->thread, NULL);
}

void reac_slave_close(struct reac_slave *s)
{
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
}
