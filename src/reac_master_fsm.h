// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_master_fsm — the MASTER establishment transition decisions as a PURE
 * table (no I/O, no struct reac_master access), the shape reac_fsm.h already
 * gave the slave side. docs/MASTER-FSM.md is the spec: every row of the table
 * is a row of that document, and the document's rows were read from the
 * pre-table code — so the table changes NOTHING, it only makes the decisions
 * explicit and exhaustively assertable (tests/test_reac_master_fsm.c pins
 * every (state, event) pair, ignores included).
 *
 * Division of labour (#61):
 *   - reac_master_rx / reac_master_next are EVENT PRODUCERS: they resolve the
 *     guards (block present? same box MAC? full ENROLL+dwell+burst delivered?
 *     link budget drained?) into one reac_master_ev via
 *     reac_master_fsm_classify + their own timer checks;
 *   - reac_master_fsm_step is the DECISION: (state, event) -> the edge;
 *   - the enter_*() functions in reac_master.c remain the ENTRY ACTIONS,
 *     run by the producer when the edge says so.
 */
#ifndef REAC_MASTER_FSM_H
#define REAC_MASTER_FSM_H

#include "reac_master.h"   /* reac_master_state / rx_event / drop_reason */

/* The classified decision events. The rx kinds fold their guards in (so the
 * step function stays a pure pair lookup); the timer kinds are produced by
 * reac_master_next's per-slot checks. */
enum reac_master_ev {
	REAC_M_EV_START = 0,      /* first pacer slot, or any RX while IDLE       */
	REAC_M_EV_PRESENCE,       /* bcast presence flood, or a JOIN without its
	                           * block — never grants (the anti-#130 rule)    */
	REAC_M_EV_JOIN_NEW,       /* validated JOIN (block present) from a box
	                           * whose MAC differs from the latched box_mac   */
	REAC_M_EV_JOIN_SAME,      /* validated JOIN retry from the latched box    */
	REAC_M_EV_CONFIG,         /* box config-announce, full grant delivered    */
	REAC_M_EV_CONFIG_EARLY,   /* box config-announce, grant not yet delivered */
	REAC_M_EV_ACCEPT,         /* box unicast/heartbeat, full grant delivered  */
	REAC_M_EV_ACCEPT_EARLY,   /* box unicast/heartbeat before full delivery   */
	REAC_M_EV_BYE,            /* box heartbeat with selector 0x00             */
	REAC_M_EV_GRANT_DELIVERED,/* tick: ENROLL + dwell + full sweep emitted    */
	REAC_M_EV_LINK_LOST,      /* tick: established link-check budget drained  */
	REAC_M_EV_COUNT
};

/* One edge of the transition table. `enter` says whether the producer must run
 * the entry action for `next` (a same-state re-entry — GRANTING re-latching a
 * different box — is a real entry); `drop` is the reason to latch into
 * m->drop_reason (DROP_NONE = leave it); `transitioned` is reac_master_rx's
 * return-value contract (the IDLE->PROBING promotion enters but reports 0,
 * exactly as the pre-table code did). */
struct reac_master_edge {
	enum reac_master_state       next;
	int                          enter;
	enum reac_master_drop_reason drop;
	int                          transitioned;
};

/* Fold an RX event + its resolved guards into the decision event. PURE.
 * `have_blk`: the 32-byte control block accompanied the frame (JOIN without it
 * is mere presence). `same_box`: the frame's L2 source equals the latched
 * box_mac. `grant_delivered`: grant_ticks has covered ENROLL + dwell + the
 * full sweep (the accept gate; meaningless outside GRANTING and ignored by
 * every other state's row). */
enum reac_master_ev reac_master_fsm_classify(enum reac_master_rx_event ev,
                                             int have_blk, int same_box,
                                             int grant_delivered);

/* The decision: (state, event) -> edge. PURE lookup, total over the enums;
 * out-of-range input returns a stay-put ignore edge. */
struct reac_master_edge reac_master_fsm_step(enum reac_master_state state,
                                             enum reac_master_ev ev);

#endif /* REAC_MASTER_FSM_H */
