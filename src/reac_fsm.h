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
 * FLOOD_ANNOUNCE presence announcement (#130, byte-verified 2026-07-11): a real
 * box announces by FLOODING broadcast FILLER at wire rate on PHY-up, but that
 * flood is BOUNDED — 5459 frames over ~1.36 s on a cold boot (§13p.3), then the
 * box STOPS broadcasting entirely and goes UNICAST-ONLY. It does NOT emit a
 * cold-connect alongside the flood; the cold-connect (cdea 04 03) is unicast to
 * the master AFTER the bounded flood, on a ~100 ms retry grid interleaved with
 * unicast audio FILLER, and the master grants ~1.7 s after broadcast stops
 * (m200-s1608-realbox-establish-2026-07-11.pcap). So the establishment is two
 * distinct TX phases: FSM_FLOOD_ANNOUNCE (bounded broadcast flood, learn the
 * master MAC) -> FSM_COLDCONNECT (unicast cold-connect on the retry grid +
 * unicast audio between) -> grant -> TX_MUTE. FSM_ACT_FLOOD_BCAST fires on every
 * FLOOD_ANNOUNCE step; FSM_ACT_UNICAST_COLDCONNECT on every FSM_COLDCONNECT step,
 * with the `emit_join` side flag (mirroring `emit_heartbeat` in ESTABLISHED)
 * marking the grid steps that carry the cdea 04 03 rather than an audio FILLER. */
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

/* Bounded presence-flood length: how many broadcast FILLER frames the box floods
 * on PHY-up before it stops broadcasting and switches to the unicast cold-connect
 * phase. Byte-verified 2026-07-11 at 48k (§13p.3: 5459 frames over ~1.36 s at the
 * 48k box rate = sampleRate/12 ≈ 4000 fps). Whether this frame-count SCALES with
 * the wire rate (i.e. is really a ~1.36 s wall-clock window) is rig-TBD — a
 * frame-count bound is faithful at 48k and safe elsewhere. */
#define REAC_FSM_FLOOD_BURST      5460

/* The unicast cold-connect retry grid (FSM_COLDCONNECT): emit one cdea 04 03
 * every REAC_FSM_JOIN_RETRY_PERIOD steps, unicast audio FILLER between, until the
 * master's grant lands (no hard give-up while PHY stays up). Reuses the
 * TXMUTE_DWELL magnitude (~100 ms @8000fps) — the box's documented ~100 ms grid. */
#define REAC_FSM_JOIN_RETRY_PERIOD 800    /* ~100ms @8000fps */

enum reac_fsm_state {
	FSM_PHY_DOWN = 0,
	FSM_FLOOD_ANNOUNCE,   /* hunting: BOUNDED broadcast FILLER flood, learn master */
	FSM_COLDCONNECT,      /* flood done: unicast cold-connect on the retry grid */
	FSM_TX_MUTE,          /* grant accepted, settle dwell */
	FSM_ESTABLISHED,      /* linked: unicast audio + heartbeat */
	FSM_DROP,             /* link lost / torn down */
};

enum reac_fsm_action {
	FSM_ACT_NONE = 0,
	FSM_ACT_FLOOD_BCAST,        /* broadcast FILLER while announcing (bounded) */
	FSM_ACT_UNICAST_COLDCONNECT,/* unicast cold-connect grid (+cdea on emit_join) */
	FSM_ACT_SILENCE,            /* mute window: counter free-runs, audio held */
	FSM_ACT_UNICAST_AUDIO,      /* established: unicast upstream FILLER (+heartbeat) */
	FSM_ACT_STOP,               /* idle, emit nothing */
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
	int      flood_frames;     /* broadcast FILLER frames flooded so far (bounded) */
	enum reac_fsm_drop_reason drop_reason;
	int      emit_heartbeat;   /* set on the step that should emit a keep-alive */
	int      emit_join;        /* set on the COLDCONNECT step that emits the cdea 04 03 */
	int      join_retry_countdown;  /* steps until the next cold-connect on the grid */
};

struct reac_fsm_out {
	enum reac_fsm_action action;
	enum reac_fsm_state  state;
	int emit_heartbeat;        /* 1 if the action should be accompanied by a heartbeat */
	int emit_join;             /* 1 if a COLDCONNECT step carries the cdea 04 03 (else audio) */
};

void reac_fsm_init(struct reac_fsm *fsm);

/* Advance the machine. For FSM_EV_RX, `rx` must point to the parsed frame; it is
 * ignored (may be NULL) for other events. Pure: mutates only *fsm. */
struct reac_fsm_out reac_fsm_step(struct reac_fsm *fsm, enum reac_fsm_event ev,
                                  const struct reac_ctrl_parsed *rx);

#endif /* REAC_FSM_H */
