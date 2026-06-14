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
	struct reac_fsm_out o = { a, fsm->state, fsm->emit_heartbeat };
	fsm->emit_heartbeat = 0;
	return o;
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
			return out(fsm, FSM_ACT_FLOOD_BCAST);
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
			return out(fsm, FSM_ACT_EMIT_JOIN);
		}
		/* tick: keep flooding + periodically re-emit the cold-connect burst */
		fsm->counter++;
		return out(fsm, FSM_ACT_EMIT_JOIN);

	case FSM_TX_MUTE:
		if (ev == FSM_EV_TICK) {
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
			/* PHY still up after a drop -> re-announce */
			fsm->state = FSM_FLOOD_ANNOUNCE;
			fsm->have_master = 0;
			return out(fsm, FSM_ACT_FLOOD_BCAST);
		}
		return out(fsm, FSM_ACT_STOP);
	}
	return out(fsm, FSM_ACT_STOP);
}
