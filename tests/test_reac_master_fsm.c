// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The MASTER decision core, exhaustively: every (state, event) pair — all
 * 4 x 11 = 44 — asserted against docs/MASTER-FSM.md, IGNORES INCLUDED, plus
 * the classifier's full (rx kind x guards) fold. The expectations here are
 * written out from the document (the pre-table code's behaviour), NOT read
 * back from the table, so a table edit that silently flips an edge fails
 * here even though the byte goldens (which only exercise the golden path)
 * might not reach it. */
#include <reac/reac_master_fsm.h>

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* One pair: step (s, ev) and assert the full edge. */
static int pair(enum reac_master_state s, enum reac_master_ev ev,
                enum reac_master_state next, int enter,
                enum reac_master_drop_reason drop, int transitioned, int line)
{
	struct reac_master_edge e = reac_master_fsm_step(s, ev);
	if (e.next != next || e.enter != enter || e.drop != drop ||
	    e.transitioned != transitioned) {
		fprintf(stderr, "FAIL: pair(state=%d, ev=%d) -> {next=%d enter=%d "
		        "drop=%d ret=%d}, expected {%d %d %d %d} (line %d)\n",
		        (int)s, (int)ev, (int)e.next, e.enter, (int)e.drop,
		        e.transitioned, (int)next, enter, (int)drop, transitioned,
		        line);
		return 1;
	}
	return 0;
}

#define HOLDS(s, ev)                if (pair((s), (ev), (s), 0, REAC_M_DROP_NONE, 0, __LINE__)) return 1
#define GOES(s, ev, next, drop, ret) if (pair((s), (ev), (next), 1, (drop), (ret), __LINE__)) return 1

int main(void)
{
	/* ---- IDLE: only START moves it (the promotion — enters PROBING but
	 * reports no transition, the pre-table rx return contract); every other
	 * event holds, because the producers always promote first. */
	GOES(REAC_M_IDLE, REAC_M_EV_START, REAC_M_PROBING, REAC_M_DROP_NONE, 0);
	HOLDS(REAC_M_IDLE, REAC_M_EV_PRESENCE);
	HOLDS(REAC_M_IDLE, REAC_M_EV_JOIN_NEW);
	HOLDS(REAC_M_IDLE, REAC_M_EV_JOIN_SAME);
	HOLDS(REAC_M_IDLE, REAC_M_EV_CONFIG);
	HOLDS(REAC_M_IDLE, REAC_M_EV_CONFIG_EARLY);
	HOLDS(REAC_M_IDLE, REAC_M_EV_ACCEPT);
	HOLDS(REAC_M_IDLE, REAC_M_EV_ACCEPT_EARLY);
	HOLDS(REAC_M_IDLE, REAC_M_EV_BYE);
	HOLDS(REAC_M_IDLE, REAC_M_EV_GRANT_DELIVERED);
	HOLDS(REAC_M_IDLE, REAC_M_EV_LINK_LOST);

	/* ---- PROBING: only a validated JOIN (either MAC — after a drop the
	 * SAME box's re-JOIN re-courts) or a config-announce (warm relink, both
	 * gate values — the delivered predicate is stale here) leave. Presence /
	 * accepts / BYE / timers never move it (anti-#130: presence never
	 * grants; no timer path out). */
	HOLDS(REAC_M_PROBING, REAC_M_EV_START);
	HOLDS(REAC_M_PROBING, REAC_M_EV_PRESENCE);
	GOES(REAC_M_PROBING, REAC_M_EV_JOIN_NEW,  REAC_M_GRANTING, REAC_M_DROP_NONE, 1);
	GOES(REAC_M_PROBING, REAC_M_EV_JOIN_SAME, REAC_M_GRANTING, REAC_M_DROP_NONE, 1);
	GOES(REAC_M_PROBING, REAC_M_EV_CONFIG,       REAC_M_GRANTING, REAC_M_DROP_NONE, 1);
	GOES(REAC_M_PROBING, REAC_M_EV_CONFIG_EARLY, REAC_M_GRANTING, REAC_M_DROP_NONE, 1);
	HOLDS(REAC_M_PROBING, REAC_M_EV_ACCEPT);
	HOLDS(REAC_M_PROBING, REAC_M_EV_ACCEPT_EARLY);
	HOLDS(REAC_M_PROBING, REAC_M_EV_BYE);
	HOLDS(REAC_M_PROBING, REAC_M_EV_GRANT_DELIVERED);
	HOLDS(REAC_M_PROBING, REAC_M_EV_LINK_LOST);

	/* ---- GRANTING: a different box re-latches (re-entry runs the entry
	 * action — fresh window); the same box's retry grid holds the dwell;
	 * accepts establish only when delivered (early ones hold — the ~1-frame
	 * burst cut / partial head-amp scene rig failures); BYE drops; the
	 * delivered tick self-completes. */
	HOLDS(REAC_M_GRANTING, REAC_M_EV_START);
	HOLDS(REAC_M_GRANTING, REAC_M_EV_PRESENCE);
	GOES(REAC_M_GRANTING, REAC_M_EV_JOIN_NEW, REAC_M_GRANTING, REAC_M_DROP_NONE, 1);
	HOLDS(REAC_M_GRANTING, REAC_M_EV_JOIN_SAME);
	GOES(REAC_M_GRANTING, REAC_M_EV_CONFIG, REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1);
	HOLDS(REAC_M_GRANTING, REAC_M_EV_CONFIG_EARLY);
	GOES(REAC_M_GRANTING, REAC_M_EV_ACCEPT, REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1);
	HOLDS(REAC_M_GRANTING, REAC_M_EV_ACCEPT_EARLY);
	GOES(REAC_M_GRANTING, REAC_M_EV_BYE, REAC_M_PROBING, REAC_M_DROP_BYE, 1);
	GOES(REAC_M_GRANTING, REAC_M_EV_GRANT_DELIVERED, REAC_M_ESTABLISHED, REAC_M_DROP_NONE, 1);
	HOLDS(REAC_M_GRANTING, REAC_M_EV_LINK_LOST);

	/* ---- ESTABLISHED: BYE / drained budget drop to PROBING (with their
	 * reasons); a different box's JOIN re-courts (MAC change); the same box's
	 * settling JOINs and everything else hold the lock. */
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_START);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_PRESENCE);
	GOES(REAC_M_ESTABLISHED, REAC_M_EV_JOIN_NEW, REAC_M_GRANTING, REAC_M_DROP_MAC_CHANGE, 1);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_JOIN_SAME);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_CONFIG);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_CONFIG_EARLY);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_ACCEPT);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_ACCEPT_EARLY);
	GOES(REAC_M_ESTABLISHED, REAC_M_EV_BYE, REAC_M_PROBING, REAC_M_DROP_BYE, 1);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_GRANT_DELIVERED);
	GOES(REAC_M_ESTABLISHED, REAC_M_EV_LINK_LOST, REAC_M_PROBING, REAC_M_DROP_PEER_GONE, 1);

	/* ---- out-of-range input: a stay-put ignore, never a wild table read. */
	HOLDS((enum reac_master_state)99, REAC_M_EV_BYE);
	HOLDS(REAC_M_ESTABLISHED, (enum reac_master_ev)99);
	HOLDS(REAC_M_ESTABLISHED, REAC_M_EV_COUNT);

	/* ---- the classifier fold: every rx kind x guard combination. ---------- */

	/* presence flood is presence, whatever the guards say */
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_BCAST_FILLER, 0, 0, 0) == REAC_M_EV_PRESENCE);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_BCAST_FILLER, 1, 1, 1) == REAC_M_EV_PRESENCE);

	/* JOIN: no block = presence (never latches); with block, the MAC compare
	 * splits new/same; the delivered gate is irrelevant to a JOIN */
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 0, 0, 0) == REAC_M_EV_PRESENCE);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 0, 1, 1) == REAC_M_EV_PRESENCE);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 1, 0, 0) == REAC_M_EV_JOIN_NEW);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 1, 0, 1) == REAC_M_EV_JOIN_NEW);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 1, 1, 0) == REAC_M_EV_JOIN_SAME);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_JOIN, 1, 1, 1) == REAC_M_EV_JOIN_SAME);

	/* CONFIG splits on the delivered gate only */
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_CONFIG, 0, 0, 0) == REAC_M_EV_CONFIG_EARLY);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_CONFIG, 1, 1, 0) == REAC_M_EV_CONFIG_EARLY);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_CONFIG, 0, 0, 1) == REAC_M_EV_CONFIG);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_CONFIG, 1, 1, 1) == REAC_M_EV_CONFIG);

	/* UNICAST and HEARTBEAT fold together: both are the box's accept, split
	 * on the delivered gate only */
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_UNICAST,   0, 0, 0) == REAC_M_EV_ACCEPT_EARLY);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_UNICAST,   1, 1, 1) == REAC_M_EV_ACCEPT);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_HEARTBEAT, 0, 0, 0) == REAC_M_EV_ACCEPT_EARLY);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_HEARTBEAT, 1, 1, 1) == REAC_M_EV_ACCEPT);

	/* BYE is BYE, whatever the guards say */
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_BYE, 0, 0, 0) == REAC_M_EV_BYE);
	CHK(reac_master_fsm_classify(REAC_M_RX_BOX_BYE, 1, 1, 1) == REAC_M_EV_BYE);

	printf("OK: master decision core — all 44 (state, event) pairs asserted "
	       "(ignores included), out-of-range stays put, classifier fold "
	       "covers every rx kind x guard combination\n");
	return 0;
}
