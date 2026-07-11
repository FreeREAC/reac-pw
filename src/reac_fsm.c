// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_fsm.h"
#include <string.h>

/* heartbeat ~1/s: emit one keep-alive per this many established frames.
 * Frame-count gate, not wall-clock (the spec's HOLD model). Conservative ~8000. */
#define HEARTBEAT_PERIOD 8000

void reac_fsm_init(struct reac_fsm *fsm)
{
	memset(fsm, 0, sizeof *fsm);
	fsm->state = FSM_PHY_DOWN;
}

static int is_master_frame(const struct reac_ctrl_parsed *rx)
{
	return rx->kind == REAC_CTRL_MASTER_HB || rx->kind == REAC_CTRL_MASTER_ANNOUNCE ||
	       rx->kind == REAC_CTRL_PROBE || rx->kind == REAC_CTRL_GRANT;
}

static void learn_master(struct reac_fsm *fsm, const struct reac_ctrl_parsed *rx)
{
	memcpy(fsm->master_mac, rx->src, 6);   /* from L2 source — unambiguous */
	fsm->have_master = 1;
}

static struct reac_fsm_out out(struct reac_fsm *fsm, enum reac_fsm_action a)
{
	struct reac_fsm_out o = { a, fsm->state, fsm->emit_heartbeat, fsm->emit_join };
	fsm->emit_heartbeat = 0;
	fsm->emit_join = 0;
	return o;
}

/* Enter FLOOD_ANNOUNCE: begin the bounded broadcast presence-flood from zero. */
static void arm_flood(struct reac_fsm *fsm)
{
	fsm->state = FSM_FLOOD_ANNOUNCE;
	fsm->flood_frames = 0;
}

/* Arm the unicast cold-connect grid so the FIRST FSM_COLDCONNECT step emits the
 * cdea 04 03 (countdown drains to <=0 immediately), then one per RETRY_PERIOD. */
static void arm_coldconnect(struct reac_fsm *fsm)
{
	fsm->join_retry_countdown = 0;
}

/* One FLOOD_ANNOUNCE tick: emit ONE broadcast FILLER frame (no cold-connect
 * alongside — the flood is broadcast-only, §13p.3) and count it toward the bound.
 * The caller decides the flood->cold-connect handoff after this runs. */
static struct reac_fsm_out flood_tick(struct reac_fsm *fsm)
{
	fsm->counter++;
	if (fsm->flood_frames < REAC_FSM_FLOOD_BURST)
		fsm->flood_frames++;
	return out(fsm, FSM_ACT_FLOOD_BCAST);
}

/* One FSM_COLDCONNECT tick: emit ONE unicast frame to the master. On the retry
 * grid (every REAC_FSM_JOIN_RETRY_PERIOD steps) it carries the cdea 04 03
 * cold-connect (emit_join), otherwise a unicast audio FILLER — one frame per
 * counter value either way. */
static struct reac_fsm_out coldconnect_tick(struct reac_fsm *fsm)
{
	fsm->counter++;
	if (--fsm->join_retry_countdown <= 0) {
		fsm->emit_join = 1;
		fsm->join_retry_countdown = REAC_FSM_JOIN_RETRY_PERIOD;
	}
	return out(fsm, FSM_ACT_UNICAST_COLDCONNECT);
}

struct reac_fsm_out reac_fsm_step(struct reac_fsm *fsm, enum reac_fsm_event ev,
                                  const struct reac_ctrl_parsed *rx)
{
	/* PHY transitions are global. */
	if (ev == FSM_EV_PHY_DOWN) {
		fsm->state = FSM_PHY_DOWN;
		fsm->have_master = 0;
		return out(fsm, FSM_ACT_STOP);
	}

	switch (fsm->state) {
	case FSM_PHY_DOWN:
		if (ev == FSM_EV_PHY_UP) {
			arm_flood(fsm);
			fsm->counter = 0;
			return flood_tick(fsm);
		}
		return out(fsm, FSM_ACT_STOP);

	case FSM_FLOOD_ANNOUNCE:
		if (ev == FSM_EV_RX && rx) {
			if (is_master_frame(rx))
				learn_master(fsm, rx);
			if (rx->kind == REAC_CTRL_GRANT) {     /* JOIN gate (early grant) */
				fsm->state = FSM_TX_MUTE;
				fsm->txmute_dwell = REAC_FSM_TXMUTE_DWELL;
				fsm->link_check = REAC_FSM_LINKCHECK_RELOAD;
				return out(fsm, FSM_ACT_SILENCE);
			}
		}
		/* tick or non-grant RX: flood one frame. Once the bounded burst is done
		 * AND the master MAC is learned, stop broadcasting and hand off to the
		 * unicast cold-connect phase — the box goes unicast-only (§13p.3). */
		{
			struct reac_fsm_out o = flood_tick(fsm);
			if (fsm->flood_frames >= REAC_FSM_FLOOD_BURST && fsm->have_master) {
				fsm->state = FSM_COLDCONNECT;
				arm_coldconnect(fsm);
			}
			return o;
		}

	case FSM_COLDCONNECT:
		if (ev == FSM_EV_RX && rx) {
			if (is_master_frame(rx))
				learn_master(fsm, rx);
			if (rx->kind == REAC_CTRL_GRANT) {     /* JOIN gate */
				fsm->state = FSM_TX_MUTE;
				fsm->txmute_dwell = REAC_FSM_TXMUTE_DWELL;
				fsm->link_check = REAC_FSM_LINKCHECK_RELOAD;
				return out(fsm, FSM_ACT_SILENCE);
			}
		}
		if (!fsm->have_master) {          /* master vanished -> re-flood broadcast */
			arm_flood(fsm);
			return flood_tick(fsm);
		}
		/* tick or non-grant RX: unicast cold-connect on the grid, audio between */
		return coldconnect_tick(fsm);

	case FSM_TX_MUTE:
		/* Frame-arrival IS the box's clock (it recovers word clock from the
		 * master's inter-arrival interval), so a received master frame
		 * advances the dwell exactly like a self-clocked tick. Without this
		 * a flooding master (8000 fps — the normal case) starves the timeout
		 * path and the dwell never elapses. */
		if (ev == FSM_EV_TICK || ev == FSM_EV_RX) {
			fsm->counter++;
			if (--fsm->txmute_dwell <= 0) {
				fsm->state = FSM_ESTABLISHED;
				fsm->link_check = REAC_FSM_LINKCHECK_RELOAD;
				fsm->heartbeat_tick = 0;
				return out(fsm, FSM_ACT_UNICAST_AUDIO);
			}
		}
		return out(fsm, FSM_ACT_SILENCE);

	case FSM_ESTABLISHED:
		if (ev == FSM_EV_RX && rx) {
			if (fsm->have_master && memcmp(rx->src, fsm->master_mac, 6) != 0 &&
			    is_master_frame(rx)) {
				fsm->state = FSM_DROP; fsm->drop_reason = FSM_DROP_MAC_CHANGE;
				return out(fsm, FSM_ACT_STOP);
			}
			if (rx->kind == REAC_CTRL_MASTER_HB || rx->kind == REAC_CTRL_MASTER_ANNOUNCE)
				fsm->link_check = REAC_FSM_LINKCHECK_RELOAD;   /* re-arm peer-alive */
			/* NOTE: an explicit-disconnect signal exists but its byte position is
			 * NOT settled — the captured master HB carries a channel# at [22], not
			 * a keep-alive flag. Drop is driven by the peer-gone countdown +
			 * MAC-change, which are sound. Wire the explicit signal after a rig
			 * capture identifies the real byte (REAC-CONNECTION-FSM.md gap list). */
			/* The ~1/s keep-alive counts frame PERIODS, and with a flooding
			 * master every period carries a frame (no timeout ticks) — so the
			 * heartbeat cadence must advance on RX too, like a real box's. */
			if (++fsm->heartbeat_tick >= HEARTBEAT_PERIOD) {
				fsm->heartbeat_tick = 0;
				fsm->emit_heartbeat = 1;
			}
			return out(fsm, FSM_ACT_UNICAST_AUDIO);
		}
		/* tick: stream upstream audio, decrement peer-alive, heartbeat ~1/s */
		fsm->counter++;
		if (--fsm->link_check <= 0) {
			fsm->state = FSM_DROP; fsm->drop_reason = FSM_DROP_PEER_GONE;
			return out(fsm, FSM_ACT_STOP);
		}
		if (++fsm->heartbeat_tick >= HEARTBEAT_PERIOD) {
			fsm->heartbeat_tick = 0;
			fsm->emit_heartbeat = 1;
		}
		return out(fsm, FSM_ACT_UNICAST_AUDIO);

	case FSM_DROP:
		if (ev == FSM_EV_PHY_UP || ev == FSM_EV_TICK) {
			/* PHY still up after a drop -> re-announce (fresh bounded flood) */
			fsm->have_master = 0;
			arm_flood(fsm);
			return flood_tick(fsm);
		}
		return out(fsm, FSM_ACT_STOP);
	}
	return out(fsm, FSM_ACT_STOP);
}
