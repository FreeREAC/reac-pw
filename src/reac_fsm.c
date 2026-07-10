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

/* Arm a fresh cold-connect burst: REAC_FSM_JOIN_BURST_COUNT frames back to
 * back, starting on THIS tick. */
static void arm_join_burst(struct reac_fsm *fsm)
{
	fsm->join_burst_left = REAC_FSM_JOIN_BURST_COUNT;
	fsm->join_retry_countdown = 0;
}

/* One FLOOD_ANNOUNCE tick: always flood (the continuous presence announcement,
 * §13p.3), and drive the cold-connect burst/retry-grid on top of it via the
 * emit_join side flag — an immediate burst of REAC_FSM_JOIN_BURST_COUNT
 * frames, then one more burst every REAC_FSM_JOIN_RETRY_PERIOD ticks,
 * unbounded, until the master's grant is RX'd (handled by the caller before
 * this runs). Mirrors the emit_heartbeat pattern used in ESTABLISHED. */
static struct reac_fsm_out flood_tick(struct reac_fsm *fsm)
{
	fsm->counter++;
	if (fsm->join_burst_left > 0) {
		fsm->emit_join = 1;
		fsm->join_burst_left--;
		if (fsm->join_burst_left == 0)
			fsm->join_retry_countdown = REAC_FSM_JOIN_RETRY_PERIOD;
	} else if (--fsm->join_retry_countdown <= 0) {
		fsm->emit_join = 1;
		fsm->join_burst_left = REAC_FSM_JOIN_BURST_COUNT - 1;  /* this tick is burst frame 1 */
		if (fsm->join_burst_left == 0)
			fsm->join_retry_countdown = REAC_FSM_JOIN_RETRY_PERIOD;
	}
	return out(fsm, FSM_ACT_FLOOD_BCAST);
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
			fsm->state = FSM_FLOOD_ANNOUNCE;
			fsm->counter = 0;
			arm_join_burst(fsm);
			return flood_tick(fsm);
		}
		return out(fsm, FSM_ACT_STOP);

	case FSM_FLOOD_ANNOUNCE:
		if (ev == FSM_EV_RX && rx) {
			if (is_master_frame(rx))
				learn_master(fsm, rx);
			if (rx->kind == REAC_CTRL_GRANT) {     /* JOIN gate */
				fsm->state = FSM_TX_MUTE;
				fsm->txmute_dwell = REAC_FSM_TXMUTE_DWELL;
				fsm->link_check = REAC_FSM_LINKCHECK_RELOAD;
				return out(fsm, FSM_ACT_SILENCE);
			}
			return flood_tick(fsm);
		}
		/* tick: keep flooding + periodically re-emit the cold-connect burst */
		return flood_tick(fsm);

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
			/* PHY still up after a drop -> re-announce (fresh flood + burst) */
			fsm->state = FSM_FLOOD_ANNOUNCE;
			fsm->have_master = 0;
			arm_join_burst(fsm);
			return flood_tick(fsm);
		}
		return out(fsm, FSM_ACT_STOP);
	}
	return out(fsm, FSM_ACT_STOP);
}
