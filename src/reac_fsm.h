// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_fsm — the virtual-stagebox JOIN/HOLD state machine as a PURE function
 * (no I/O), so it is fully offline-testable. The caller does the I/O: it parses
 * received frames with reac_ctrl_parse(), drives reac_fsm_step() with events,
 * and acts on the returned action (flood broadcast / unicast audio + heartbeat
 * / go silent / stop). See REAC-CONNECTION-FSM.md.
 *
 * HOLD (ESTABLISHED keep-alive + drop logic) is validated against the real
 * captures. JOIN (the grant transition) is reconstructed and only reaches
 * ESTABLISHED on a real master's grant frame — gated experimental until a rig
 * capture confirms the grant-burst bytes.
 *
 * FLOOD_ANNOUNCE presence announcement (#130 fix 1): a real box announces by
 * FLOODING broadcast FILLER at wire rate on PHY-up (§13p.3: 5459 frames over
 * ~1.36 s on a cold boot) — that flood is what makes a real master register
 * and DISPLAY the box. The box's own cdea 04 03 cold-connect is also on the
 * wire (§13p.multi, byte-captured) but as a burst of a few frames plus a
 * ~100 ms retry grid, never as a continuous replacement for the flood. So
 * FSM_ACT_FLOOD_BCAST fires on EVERY FLOOD_ANNOUNCE step (the continuous
 * presence-flood); the `emit_join` side flag (mirroring `emit_heartbeat` in
 * ESTABLISHED) marks the steps that should ALSO carry a cold-connect frame:
 * an immediate burst of REAC_FSM_JOIN_BURST_COUNT frames, then one retry
 * burst every REAC_FSM_JOIN_RETRY_PERIOD ticks, unbounded, until the master's
 * grant lands (no hard give-up while PHY stays up). */
#ifndef REAC_FSM_H
#define REAC_FSM_H

#include <stdint.h>
#include "reac_ctrl.h"

#define REAC_FSM_LINKCHECK_RELOAD 600    /* 0x0258 frames */
/* TX-mute settle dwell after the grant, in frame periods. PLACEHOLDER pending
 * a rig capture — the golden transcript says the box switches to unicast "the
 * instant the grant lands", so short is faithful; it MUST stay well under the
 * master's ~150 ms grant window (fps*15/100 slots) or the master's window
 * expires before our first unicast and the courtship never closes. */
#define REAC_FSM_TXMUTE_DWELL     800    /* ~100ms @8000fps */

/* The cold-connect JOIN burst that rides the presence-flood: a short burst on
 * entry/each retry, then a retry grid — NOT a continuous per-tick spam (the
 * #130 fix-1 defect this replaces). REAC_FSM_JOIN_RETRY_PERIOD reuses the
 * TXMUTE_DWELL magnitude (~100 ms @8000fps ticks) — the same order as the
 * master's own ~150 ms grant window / the box's documented ~100 ms retry grid. */
#define REAC_FSM_JOIN_BURST_COUNT    3
#define REAC_FSM_JOIN_RETRY_PERIOD 800    /* ~100ms @8000fps */

enum reac_fsm_state {
	FSM_PHY_DOWN = 0,
	FSM_FLOOD_ANNOUNCE,   /* hunting: broadcast FILLER flood (+ the JOIN burst) */
	FSM_TX_MUTE,          /* grant accepted, settle dwell */
	FSM_ESTABLISHED,      /* linked: unicast audio + heartbeat */
	FSM_DROP,             /* link lost / torn down */
};

enum reac_fsm_action {
	FSM_ACT_NONE = 0,
	FSM_ACT_FLOOD_BCAST,  /* broadcast FILLER while announcing (continuous) */
	FSM_ACT_SILENCE,      /* mute window: counter free-runs, audio held */
	FSM_ACT_UNICAST_AUDIO,/* established: unicast upstream FILLER (+heartbeat on tick) */
	FSM_ACT_STOP,         /* idle, emit nothing */
};

enum reac_fsm_event {
	FSM_EV_PHY_UP = 0,
	FSM_EV_PHY_DOWN,
	FSM_EV_RX,            /* a parsed received frame is supplied */
	FSM_EV_TICK,          /* one frame-period elapsed */
};

enum reac_fsm_drop_reason {
	FSM_DROP_NONE = 0, FSM_DROP_EXPLICIT, FSM_DROP_PEER_GONE, FSM_DROP_MAC_CHANGE,
};

struct reac_fsm {
	enum reac_fsm_state state;
	uint8_t  master_mac[6];
	int      have_master;
	int      link_check;       /* countdown to peer-gone */
	int      txmute_dwell;
	int      heartbeat_tick;   /* frames since our last heartbeat */
	uint16_t counter;          /* our free-running u16-LE */
	enum reac_fsm_drop_reason drop_reason;
	int      emit_heartbeat;   /* set on the step that should emit a keep-alive */
	int      emit_join;        /* set on the step that should also emit a cold-connect */
	int      join_burst_left;  /* cold-connect frames left in the current burst */
	int      join_retry_countdown;  /* ticks until the next burst may start */
};

struct reac_fsm_out {
	enum reac_fsm_action action;
	enum reac_fsm_state  state;
	int emit_heartbeat;        /* 1 if the action should be accompanied by a heartbeat */
	int emit_join;             /* 1 if a FLOOD_BCAST step should also emit a cold-connect */
};

void reac_fsm_init(struct reac_fsm *fsm);

/* Advance the machine. For FSM_EV_RX, `rx` must point to the parsed frame; it is
 * ignored (may be NULL) for other events. Pure: mutates only *fsm. */
struct reac_fsm_out reac_fsm_step(struct reac_fsm *fsm, enum reac_fsm_event ev,
                                  const struct reac_ctrl_parsed *rx);

#endif /* REAC_FSM_H */
