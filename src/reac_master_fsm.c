// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_master_fsm.h"

enum reac_master_ev reac_master_fsm_classify(enum reac_master_rx_event ev,
                                             int have_blk, int same_box,
                                             int grant_delivered)
{
	switch (ev) {
	case REAC_M_RX_BOX_BCAST_FILLER:
		return REAC_M_EV_PRESENCE;
	case REAC_M_RX_BOX_JOIN:
		if (!have_blk)
			return REAC_M_EV_PRESENCE;   /* no block = mere presence */
		return same_box ? REAC_M_EV_JOIN_SAME : REAC_M_EV_JOIN_NEW;
	case REAC_M_RX_BOX_CONFIG:
		return grant_delivered ? REAC_M_EV_CONFIG : REAC_M_EV_CONFIG_EARLY;
	case REAC_M_RX_BOX_UNICAST:
	case REAC_M_RX_BOX_HEARTBEAT:
		/* Both gate GRANTING->ESTABLISHED identically (the box's accept). */
		return grant_delivered ? REAC_M_EV_ACCEPT : REAC_M_EV_ACCEPT_EARLY;
	case REAC_M_RX_BOX_BYE:
		return REAC_M_EV_BYE;
	}
	return REAC_M_EV_PRESENCE;   /* unknown kind: presence, never a grant */
}

/* Row shorthands: HOLD = stay put, no entry action, rx reports 0;
 * GO = transition (or re-entry) running the entry action for `next`. */
#define HOLD(s)          { (s), 0, REAC_M_DROP_NONE, 0 }
#define GO(next, drop, ret) { (next), 1, (drop), (ret) }

/* The table IS docs/MASTER-FSM.md — one edge per (state, event), the ignores
 * spelled out. Rows are indexed [state][event]; both enums start at 0 and are
 * dense. */
static const struct reac_master_edge
EDGE[4 /* REAC_M_IDLE..REAC_M_ESTABLISHED */][REAC_M_EV_COUNT] = {

	/* IDLE — pacer not emitting yet. Producers always promote via EV_START
	 * before feeding any other event (reac_master_rx's "the pacer is ticking
	 * us if RX arrives" rule), so the non-START rows are the contract that a
	 * stray event cannot move an unstarted master. The promotion enters
	 * PROBING but reports no transition (the pre-table return contract). */
	[REAC_M_IDLE] = {
		[REAC_M_EV_START]           = GO(REAC_M_PROBING, REAC_M_DROP_NONE, 0),
		[REAC_M_EV_PRESENCE]        = HOLD(REAC_M_IDLE),
		[REAC_M_EV_JOIN_NEW]        = HOLD(REAC_M_IDLE),
		[REAC_M_EV_JOIN_SAME]       = HOLD(REAC_M_IDLE),
		[REAC_M_EV_CONFIG]          = HOLD(REAC_M_IDLE),
		[REAC_M_EV_CONFIG_EARLY]    = HOLD(REAC_M_IDLE),
		[REAC_M_EV_ACCEPT]          = HOLD(REAC_M_IDLE),
		[REAC_M_EV_ACCEPT_EARLY]    = HOLD(REAC_M_IDLE),
		[REAC_M_EV_BYE]             = HOLD(REAC_M_IDLE),
		[REAC_M_EV_GRANT_DELIVERED] = HOLD(REAC_M_IDLE),
		[REAC_M_EV_LINK_LOST]       = HOLD(REAC_M_IDLE),
	},

	/* PROBING — unlinked hunt. Leaves ONLY on a validated JOIN (any MAC —
	 * no same-box hold here: after a drop the SAME box's re-JOIN re-courts)
	 * or a config-announce (warm relink, either gate value: the delivered
	 * predicate is stale here). Presence/accept/BYE never move it — the
	 * anti-#130 golden rule. No timer path out. */
	[REAC_M_PROBING] = {
		[REAC_M_EV_START]           = HOLD(REAC_M_PROBING),
		[REAC_M_EV_PRESENCE]        = HOLD(REAC_M_PROBING),
		[REAC_M_EV_JOIN_NEW]        = GO(REAC_M_GRANTING, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_JOIN_SAME]       = GO(REAC_M_GRANTING, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_CONFIG]          = GO(REAC_M_GRANTING, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_CONFIG_EARLY]    = GO(REAC_M_GRANTING, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_ACCEPT]          = HOLD(REAC_M_PROBING),
		[REAC_M_EV_ACCEPT_EARLY]    = HOLD(REAC_M_PROBING),
		[REAC_M_EV_BYE]             = HOLD(REAC_M_PROBING),
		[REAC_M_EV_GRANT_DELIVERED] = HOLD(REAC_M_PROBING),
		[REAC_M_EV_LINK_LOST]       = HOLD(REAC_M_PROBING),
	},

	/* GRANTING — box latched; ENROLL + dwell + sweep in flight. A DIFFERENT
	 * box's JOIN re-latches (fresh window, entry action re-runs); the SAME
	 * box's ~100 ms-grid retries hold the dwell (rig 2026-07-12). Accepts
	 * (unicast/heartbeat/config) establish only AFTER full delivery — early
	 * ones hold (a warm-relink box unicasts from slot 0 and would cut the
	 * burst to ~1 frame; a partial head-amp scene mutes the box, rig
	 * 2026-07-23). GRANT_DELIVERED is the tick self-complete. */
	[REAC_M_GRANTING] = {
		[REAC_M_EV_START]           = HOLD(REAC_M_GRANTING),
		[REAC_M_EV_PRESENCE]        = HOLD(REAC_M_GRANTING),
		[REAC_M_EV_JOIN_NEW]        = GO(REAC_M_GRANTING, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_JOIN_SAME]       = HOLD(REAC_M_GRANTING),
		[REAC_M_EV_CONFIG]          = GO(REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_CONFIG_EARLY]    = HOLD(REAC_M_GRANTING),
		[REAC_M_EV_ACCEPT]          = GO(REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_ACCEPT_EARLY]    = HOLD(REAC_M_GRANTING),
		[REAC_M_EV_BYE]             = GO(REAC_M_PROBING, REAC_M_DROP_BYE, 1),
		[REAC_M_EV_GRANT_DELIVERED] = GO(REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1),
		[REAC_M_EV_LINK_LOST]       = HOLD(REAC_M_GRANTING),
	},

	/* ESTABLISHED — linked. The producer reloads the link budget on EVERY box
	 * RX before stepping (budget mechanics, not a decision). BYE and the
	 * drained budget drop to PROBING; a DIFFERENT box's JOIN re-courts it
	 * (MAC change); the SAME box's JOINs while it settles its TX_MUTE dwell
	 * hold the stream (rig 2026-07-12: re-granting tore it down forever). */
	[REAC_M_ESTABLISHED] = {
		[REAC_M_EV_START]           = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_PRESENCE]        = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_JOIN_NEW]        = GO(REAC_M_GRANTING, REAC_M_DROP_MAC_CHANGE, 1),
		[REAC_M_EV_JOIN_SAME]       = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_CONFIG]          = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_CONFIG_EARLY]    = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_ACCEPT]          = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_ACCEPT_EARLY]    = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_BYE]             = GO(REAC_M_PROBING, REAC_M_DROP_BYE, 1),
		[REAC_M_EV_GRANT_DELIVERED] = HOLD(REAC_M_ESTABLISHED),
		[REAC_M_EV_LINK_LOST]       = GO(REAC_M_PROBING, REAC_M_DROP_PEER_GONE, 1),
	},
};

struct reac_master_edge reac_master_fsm_step(enum reac_master_state state,
                                             enum reac_master_ev ev)
{
	if ((unsigned)state > REAC_M_ESTABLISHED ||
	    (unsigned)ev >= REAC_M_EV_COUNT) {
		struct reac_master_edge stay = HOLD(state);
		return stay;
	}
	return EDGE[state][ev];
}
