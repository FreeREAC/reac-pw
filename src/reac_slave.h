// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_slave — the SLAVE-role engine: openmixer slaved to an EXTERNAL REAC master
 * (a desk, or another box configured as master). It is the inverse of reac_pacer:
 *
 *   - reac_pacer (MASTER role): WE drive the cdea/cfea establishment + WE own the
 *     clock (a SCHED_FIFO clock_nanosleep pacer at a fixed pps); a stagebox slaves
 *     to us.
 *   - reac_slave (SLAVE role): an external master drives the establishment; WE
 *     RESPOND. We do NOT run our own pacer as the timing source — the master owns
 *     the clock, so we LOCK to the incoming master cadence: every received master
 *     downstream frame is one tick, and we emit exactly one upstream frame per tick
 *     (frame-arrival = the slot clock). This is how a real Roland box clocks itself:
 *     it recovers word clock from the master's frame inter-arrival interval (§3 /
 *     §5 link-check, REAC-PROTOCOL-AND-TESTS.md) and never imposes its own rate.
 *
 * The establishment is the gold §13d slave sequence, already encoded as the PURE
 * JOIN/HOLD state machine reac_fsm: we FLOOD broadcast FILLER while unlinked, the
 * master cycles cdea 01 sub-states then GRANTS with a cdea 04 03 burst, we settle
 * through the TX-mute dwell to ESTABLISHED, then unicast our input channels upstream
 * + a ~1/s keep-alive heartbeat, with the master heartbeat re-arming our 600-frame
 * loop-check (HOLD). reac_slave is the I/O shell around that FSM: it parses each RX
 * frame (reac_ctrl_parse), steps the FSM (reac_fsm_step), and acts on the returned
 * action by emitting the right reac_ctrl builder frame on the AF_PACKET TX socket.
 *
 * The mixer's own input channels (its contribution upstream) are pulled from a
 * planar SPSC ring the PipeWire sink fills (reac_ctrl_build_upstream_filler places
 * them at the box-width slots), exactly as a real slave sends its inputs upstream.
 * Audio FROM the master is RX'd by the existing reac:capture path (reac_rx) — the
 * two directions share the encoder/decoder + the PipeWire nodes; only WHO drives
 * the handshake + the clock differs (master drives; slave follows).
 *
 * The decision core reac_slave_step() is PURE + offline-testable, driven straight
 * from the captured slave/master frames in reac-captures. */
#ifndef REAC_SLAVE_H
#define REAC_SLAVE_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#include <reac/reac.h>   /* REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT */

#include "reac_fsm.h"
#include "reac_ring.h"
#include "reac_rt.h"

struct reac_ctrl_parsed;   /* reac_ctrl.h — a parsed received frame */

/* How many of our input channels we return upstream (a box's width: S-1608 = 16,
 * S-0808 = 8). 628 B / 340 B box-width frames per reac_ctrl_build_upstream_filler. */
#define REAC_SLAVE_BOX_CHANNELS_DEFAULT 16

struct reac_slave_cfg {
	const char *ifname;       /* the REAC NIC (raw AF_PACKET 0x8819, RX + TX) */
	int box_channels;         /* our upstream input width (<= 40); 0 -> default 16 */
	int sample_rate;          /* the master's rate (locked from the wire, not set
	                           * by us); informational for the box frame geometry */
	const uint8_t *src_mac;   /* our stable Roland-OUI src MAC; NULL -> a stand-in */
	int prio;                 /* SCHED_FIFO priority for the engine thread; 0 ->
	                           * resolved by reac_rt.h (REACPW_RT_PRIO, else the
	                           * built-in that sits BELOW the PipeWire graph) */
	/* THE PEER IS A STAGEBOX ON M, NOT A DESK (0.5.6). Two things change and
	 * nothing else does:
	 *
	 *   - `box_channels` is THE MASTER'S width, read off its broadcast, not ours.
	 *     Ground truth (`box-to-box-enroll.pcap`): a real 16-input S-1608 joining a
	 *     real S-0808 on M sent 340 B / 8 slots, not its own 628 B / 16, from its
	 *     very first flood frame. Its inputs 9-16 never reached that master at all —
	 *     the one identifiable signal on its input 9 appears in no slot of the
	 *     8-channel stream. The caller passes the width; this flag says whose it is.
	 *
	 *   - the cold-connect ORDER. To a desk the engine escalates one record per
	 *     ~100 ms grid slot (0014 -> 0013 -> 0016 -> 001a -> announce -> ...). To a
	 *     box on M the capture shows the config-announce FIRST, as the very frame
	 *     the box goes unicast with, and the cold-connect burst ~214 ms later as
	 *     three CONSECUTIVE frames (0014, 0014, 0013). The desk path is untouched. */
	int box_master;
	/* THE ONE EXPERIMENT THE TWO RIG RUNS LEFT OPEN (0.5.6-3). Two builds have been
	 * refused by the real S-0808 and they differ in two places at once:
	 *
	 *   build 1  340 B frames + a declaration derived from the MASTER's width (0x84)
	 *   build 2  1492 B frames + the S-1608's declaration, byte-identical
	 *   S-1608   340 B frames + its own declaration          -> granted in 4 ms
	 *
	 * Nobody has run the fourth corner. 0 (the default) is the operator's ruling — a
	 * mixer sends 40 channels — and 1 is an exact S-1608 imitation: 340 B at the
	 * master's width in the flood, as the carrier of the announce and the burst, and in
	 * the steady state, unicast to the box as the granted box unicast. It is a SWITCH so
	 * the rig can run both without a rebuild between them, and the box's lamp decides.
	 * `REACPW_BOX_MASTER_FRAME=box` sets it; see DESIGN.md 0.5.6. */
	int box_master_frame_box;
	/* WHAT ARMS THE COLD-CONNECT BURST (0.5.6-5). 0 (the default) is the free-running
	 * grid the granted box appeared to use: the announce, then the burst ~200 ms later,
	 * retried. 1 arms it on the box's own chanmap instead — in the ground-truth capture
	 * the S-1608's burst landed 1.0 ms after one, which is striking at n=1 and is a
	 * coincidence until a rig says otherwise. `REACPW_BOX_MASTER_BURST=chanmap` sets it,
	 * so the two can be tried in one session without a rebuild. */
	int box_master_burst_chanmap;
	/* WHAT THE SLOTS CARRY BEFORE THE GRANT (0.5.6-6). The granted S-1608's flood and
	 * pre-grant unicast carried LIVE samples in every slot; ours carry digital silence,
	 * because nothing is patched to the sink yet. This file's own flood comment says the
	 * difference matters — "on a real box the flood's audio region varies every frame" —
	 * and a box may reasonably treat a peer sending nothing at all as not really there.
	 * 1 puts -60 dBFS of noise in the slots until the grant. A HYPOTHESIS with a knob,
	 * `REACPW_BOX_MASTER_FILL=noise`; nothing measured says the box requires it. */
	int box_master_fill_noise;
	/* HOW LONG TO SAY NOTHING BEFORE THE FLOOD (0.5.6-6), milliseconds, 0 = start at
	 * once. The S-1608 was silent for about four seconds between losing its old master
	 * and beginning its flood; a box may key its enrolment window on a peer appearing
	 * out of silence. `REACPW_BOX_MASTER_PRESILENCE_MS`. Also a hypothesis. */
	int box_master_presilence_ms;
	/* WHOSE TRANSITIONS THESE ARE (0.5.6-8). The engine printed `reac_slave: STATE …`
	 * untagged, and with two box-master segments on one host the lines are
	 * unattributable — which also made a hearing test wait match another phase's
	 * ESTABLISHED and return instantly on somebody else's success. "[iface] " or ""
	 * for a lone segment, exactly as every other per-segment line is tagged. */
	const char *tag;
};

/* The slave engine. The FSM is the brain; everything else is the I/O the FSM's
 * actions drive. The master MAC is LEARNED from the wire (fsm.master_mac), never
 * configured — a box learns its master from the L2 source (reac_fsm doc). */
struct reac_slave {
	struct reac_fsm fsm;          /* the pure JOIN/HOLD slave establishment FSM */
	int box_channels;
	int sample_rate;
	uint8_t src[6];               /* our source MAC */

	int fd;                       /* AF_PACKET RX+TX socket, -1 if not open */
	int ifindex;
	int prio;                     /* the engine thread's SCHED_FIFO priority */
	enum reac_rt_prio_source prio_src;  /* which layer chose it; reported when
	                                     * the thread goes SCHED_FIFO */

	struct reac_ring *tx_ring;    /* our input channels (planar f32), filled by the
	                               * PipeWire sink; NULL -> emit silent upstream */

	pthread_t thread;
	_Atomic int running;
	_Atomic int phy_up_req;       /* submit-side requests PHY up/down; the engine
	                               * thread (the FSM owner) applies it each loop */
	int phy_up_seen;              /* engine-thread-local last-applied value */

	/* Clock-follow: our upstream counter TRACKS the master's downstream counter at
	 * a fixed offset latched at first lock (the M-200 is the word-clock master — a
	 * box whose counter free-runs/drifts is not clock-slaved and is refused). The
	 * FSM's own counter is overridden with the master-derived value before emit. */
	uint16_t counter_offset;
	int      counter_locked;      /* 1 once the offset is latched (reset on PHY-up) */
	int      coldconnect_phase;   /* cycles the cdea 04 03 escalation 0014->0013->0016->001a */
	int      box_master;          /* 0.5.6: the peer is a stagebox on M (see the cfg) */
	int      bm_frame_box;        /* imitate the box's 340 B geometry (see the cfg) */
	int      bm_burst_chanmap;    /* arm the burst on the box's chanmap (see the cfg) */
	int      bm_chanmap_hit;      /* a chanmap arrived since the last burst */
	int      bm_announced;        /* the config-announce has gone out at least once */
	int      bm_fill_noise;       /* -60 dBFS in the slots until the grant (see the cfg) */
	int      bm_presilence_ms;    /* say nothing for this long first (see the cfg) */
	char     tag[24];             /* "[iface] " for this engine's own transcript */
	/* WHAT THE MASTER IS DOING, on a box-master wire (0.5.6-9). Both settled by two
	 * captures and by replay: a master that ANNOUNCES itself is found without a flood,
	 * and a join lands after its scene transfer has stopped. Engine-thread only. */
	int      bm_saw_cfea;         /* the master announces itself: no flood is needed */
	uint64_t bm_last_scene_ns;    /* when its scene transfer last spoke */
	uint64_t bm_burst_sent_ns;    /* when the last cold-connect burst went out */
	int      bm_listened;         /* the opening listen window is over */
	int      bm_announce_sent;    /* our declaration has gone out: fillers say REQUESTING */
	int      bm_burst_just_ended; /* the next frame is the heartbeat both granted boxes send */
	uint64_t bm_start_ns;         /* when the engine began, for that silence */
	uint32_t bm_rng;              /* the noise generator's state, engine thread only */
	int      bm_seq;              /* its grid position: 0 = announce, 2 = the burst */
	int      bm_burst;            /* frames left of the 3-frame cold-connect burst */

	/* --- received head-amp -> per-input GAIN (virtual-stagebox SENS/PAD apply) ---
	 * A real box applies the console's per-channel SENS/PAD to its mic preamp
	 * BEFORE the A/D, so the analog input reaches nominal on the wire. A virtual
	 * box has no preamp, so it must apply the EQUIVALENT digital gain to the audio
	 * it returns upstream — otherwise the master's head-amp does nothing and the
	 * virtual box can't be used to test a session. We keep the raw received per-
	 * input state and a PRECOMPUTED linear gain; the RT upstream path only
	 * multiplies (no powf/alloc per frame). Indexed by OUR 0-based input index =
	 * wire CH - ch_base; records for channels outside our box are ignored. Written
	 * only by the engine/RX thread; ha_gain is published to the staging reader with
	 * a relaxed atomic store (a torn float read would be benign, but the atomic
	 * keeps it clean if staging ever moves off-thread). All-unity (1.0) until the
	 * master sends head-amp, so the upstream is byte-identical to today until then. */
	int      ch_base;                       /* our wire-channel base (S-0808 0x00, S-1608 0x20) */
	uint8_t  ha_sens[REAC_MAX_CHANNELS];    /* received SENS value 0x00..0x37 per input */
	uint8_t  ha_pad[REAC_MAX_CHANNELS];     /* received pad on/off per input */
	uint8_t  ha_phantom[REAC_MAX_CHANNELS]; /* received +48V (state only; NOT a gain) */
	_Atomic float ha_gain[REAC_MAX_CHANNELS]; /* precomputed linear input gain (relaxed) */

	/* THE LEARNED MASTER'S MAC, PUBLISHED AS ONE ATOMIC. The FSM's own copy
	 * (fsm.master_mac) is engine-thread state; the main loop publishes the
	 * segment's answer from a 200 ms timer, and reading six loose bytes across
	 * that boundary is a torn read nobody synchronises — a half-updated MAC is a
	 * WRONG answer, not merely a stale one. Packed big-endian into the low 48
	 * bits (reac_segment_ident.h's reac_mac48_pack/unpack); 0 until a master is
	 * learned, which is the fact "none" is published from. Written by the engine
	 * thread only, on the step that learns or changes the master. */
	_Atomic uint64_t master_mac48;

	/* diagnostics (read from any thread) */
	_Atomic uint64_t rx_master_frames;  /* master downstream frames we locked to */
	_Atomic uint64_t tx_frames;         /* upstream frames we emitted */
	_Atomic uint64_t tx_errors;
	_Atomic int      established;        /* 1 once the FSM reaches ESTABLISHED */
};

/* ---- the PURE decision core (no I/O; the offline-testable heart) ---------- */

/* What an established/closing slave should put on the wire for one tick. The I/O
 * loop maps this onto a reac_ctrl builder. Mirrors reac_fsm_action but at the
 * frame-emission granularity the slave TX path needs. */
enum reac_slave_emit {
	REAC_SLAVE_EMIT_NONE = 0,       /* emit nothing (PHY down / idle / mute) */
	REAC_SLAVE_EMIT_FLOOD_FILLER,   /* broadcast FILLER presence-flood (bounded announce) */
	REAC_SLAVE_EMIT_COLDCONNECT,    /* unicast cold-connect phase: cdea 04 03 on the grid
	                                 * (with_join), unicast audio FILLER between */
	REAC_SLAVE_EMIT_UPSTREAM_AUDIO, /* established: unicast our input channels up   */
	REAC_SLAVE_EMIT_HEARTBEAT,      /* established: the cdea 01 03 0001 81 keep-alive */
};

/* The decision a slave tick yields: an emit kind + a side flag selecting WHICH
 * single frame this slot carries (control REPLACES audio — exactly one frame per
 * counter value, never two), and the resolved FSM state.
 * `with_join` (valid when emit == COLDCONNECT): this slot's unicast frame is the
 * cold-connect cdea 04 03 (else a unicast audio FILLER) — after the bounded
 * broadcast flood the box goes unicast-only and cold-connects on a ~100 ms retry
 * grid (#130, byte-verified 2026-07-11). `with_heartbeat` (valid when emit ==
 * UPSTREAM_AUDIO): this slot's frame is the heartbeat, REPLACING the audio (a real
 * box's sparse keep-alive occupies an audio slot, never an extra frame). */
struct reac_slave_decision {
	enum reac_slave_emit emit;
	int with_heartbeat;          /* 1 -> this slot's audio frame is the heartbeat */
	int with_join;               /* 1 -> this slot's unicast frame is the cold-connect */
	enum reac_fsm_state state;
};

void reac_slave_fsm_init(struct reac_slave *s, const struct reac_slave_cfg *cfg);

/* ---- received head-amp -> input gain (PURE, offline-testable) -------------- */

/* The virtual-stagebox gain model: an input sensitivity of S dBu means an input
 * at S dBu reaches nominal, so the preamp gain is -S dB. reac_headamp_sens_db()
 * already folds the pad into S (pad on -> +20 dBu -> 20 dB less gain). Returns the
 * linear multiplier dbToLinear(-sens_db): higher `sens_value` (more sensitive,
 * lower dBu) -> more gain; 1 dB per value step; pad on -> 20 dB (10x) less. Pure
 * (powf only) — computed OFF the RT path, in the RX handler. */
float reac_slave_headamp_gain(uint8_t sens_value, int pad_on);

/* Ingest ONE received head-amp record (the caller must have record-checksum-
 * verified it) into our per-input state, mapping the WIRE channel back to our
 * 0-based input index (wire CH - ch_base). A record for a channel outside our box
 * (or an unknown param) is ignored. On a SENS/PAD change the per-input linear gain
 * is RECOMPUTED here (off the RT path) and published to the staging reader with a
 * relaxed atomic store; phantom is tracked as state only (a voltage, never a
 * gain). Returns our input index on an accepted record, or -1 when out of range /
 * unknown. */
int reac_slave_headamp_rx(struct reac_slave *s, const struct reac_ctrl_parsed *p);

/* RT upstream path: multiply each of `nch` planar input blocks (ns samples) by
 * its precomputed per-channel gain, IN PLACE. MULTIPLY-ONLY — no powf/alloc/
 * syscall; the per-channel gain is loaded with a relaxed atomic. A unity (1.0)
 * channel is left byte-identical. */
void reac_slave_apply_input_gain(float *const planar[], int nch, int ns,
                                 const _Atomic float *gain);

/* PURE: feed one PARSED received frame (a master broadcast/unicast we saw) into
 * the FSM and return what to emit in response. `rx` is the parsed frame. The
 * master frame is the clock tick the slave locks to (RX-driven), so this both
 * advances HOLD link-check and decides the upstream emission. */
struct reac_slave_decision reac_slave_step_rx(struct reac_slave *s,
                                              const struct reac_ctrl_parsed *rx);

/* PURE: a tick WITHOUT a received frame (PHY event or a self-clocked gap). Used to
 * drive FLOOD re-emit + the TX-mute dwell + the established peer-gone countdown
 * when the master frame interval is the clock but no parsed frame is supplied. */
struct reac_slave_decision reac_slave_step_tick(struct reac_slave *s);

/* PURE: a PHY transition (link up / down). present=1 -> PHY up (begin flooding);
 * present=0 -> PHY down (drop, stop). */
struct reac_slave_decision reac_slave_step_phy(struct reac_slave *s, int up);

/* ---- the live I/O engine (the shell around the FSM) ----------------------- */

/* Open the AF_PACKET RX+TX socket on cfg->ifname (needs CAP_NET_RAW) + init the
 * FSM. `tx_ring` carries our input channels upstream (may be NULL for silent
 * return). Returns 0 / -1. */
int  reac_slave_open(struct reac_slave *s, const struct reac_slave_cfg *cfg,
                     struct reac_ring *tx_ring);

/* Spawn the engine thread: it RXes master frames, drives the FSM, and emits the
 * upstream response locked to the master cadence. Returns 0 / -1. */
int  reac_slave_start(struct reac_slave *s);

/* Tell the engine the PHY is up/down (begin/stop the establishment). */
void reac_slave_set_phy_up(struct reac_slave *s, int up);

void reac_slave_stop(struct reac_slave *s);
void reac_slave_close(struct reac_slave *s);

#endif /* REAC_SLAVE_H */
