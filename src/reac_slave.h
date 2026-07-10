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

#include "reac_fsm.h"
#include "reac_ring.h"

/* How many of our input channels we return upstream (a box's width: S-1608 = 16,
 * S-0808 = 8). 628 B / 340 B box-width frames per reac_ctrl_build_upstream_filler. */
#define REAC_SLAVE_BOX_CHANNELS_DEFAULT 16

struct reac_slave_cfg {
	const char *ifname;       /* the REAC NIC (raw AF_PACKET 0x8819, RX + TX) */
	int box_channels;         /* our upstream input width (<= 40); 0 -> default 16 */
	int sample_rate;          /* the master's rate (locked from the wire, not set
	                           * by us); informational for the box frame geometry */
	const uint8_t *src_mac;   /* our stable Roland-OUI src MAC; NULL -> a stand-in */
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

	struct reac_ring *tx_ring;    /* our input channels (planar f32), filled by the
	                               * PipeWire sink; NULL -> emit silent upstream */

	pthread_t thread;
	_Atomic int running;
	_Atomic int phy_up_req;       /* submit-side requests PHY up/down; the engine
	                               * thread (the FSM owner) applies it each loop */
	int phy_up_seen;              /* engine-thread-local last-applied value */

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
	REAC_SLAVE_EMIT_FLOOD_FILLER,   /* broadcast FILLER presence-flood (announcing;
	                                 * continuous — see with_join below) */
	REAC_SLAVE_EMIT_UPSTREAM_AUDIO, /* established: unicast our input channels up   */
	REAC_SLAVE_EMIT_HEARTBEAT,      /* established: the cdea 01 03 0001 81 keep-alive */
};

/* The decision a slave tick yields: an emit kind + a side flag that an EXTRA
 * frame must accompany the primary emission, and the resolved FSM state.
 * `with_join` (valid when emit == FLOOD_FILLER): also send the cold-connect
 * JOIN burst frame this tick — a real box announces by flooding broadcast
 * FILLER continuously (§13p.3) while a short cold-connect burst + ~100 ms
 * retry grid rides alongside it (§13p.multi), never a cold-connect-only
 * stream (#130 fix 1). `with_heartbeat` (valid when emit ==
 * UPSTREAM_AUDIO): also emit a heartbeat alongside the audio. */
struct reac_slave_decision {
	enum reac_slave_emit emit;
	int with_heartbeat;          /* 1 -> also emit a heartbeat alongside the audio */
	int with_join;               /* 1 -> also emit the cold-connect burst this tick */
	enum reac_fsm_state state;
};

void reac_slave_fsm_init(struct reac_slave *s, const struct reac_slave_cfg *cfg);

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
