// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The full pure-FSM courtship: OUR master against OUR slave, no sockets.
 *
 * The strongest offline gate for the event-driven establishment (#130): both
 * halves are pure decision cores, so the whole M-5000 <-> S-1608 sequence runs
 * as a frame-by-frame simulation. Every master emission (reac_master_next +
 * reac_master_stamp onto a reac_tx_build FILLER) is parsed and fed into
 * reac_slave_step_rx (frame-arrival = the box's clock); every slave emission
 * (the reac_ctrl builders the live engine uses) is fed through
 * reac_ctrl_classify_box_frame + reac_master_rx (the pacer's ingest path).
 *
 * Asserted, end-to-end in the golden M-5000 ordering:
 *   1. the master probes first — no grant before the slave's cold-connect;
 *   2. the slave's presence-flood alone never grants;
 *   3. the grant appears ONLY after the slave's JOIN, and echoes its block;
 *   4. the slave reaches FSM_ESTABLISHED off the echoed grant (via TX-mute);
 *   5. the master reaches REAC_M_ESTABLISHED off the slave's first unicast;
 *   6. steady state holds >=5 simulated seconds (the slave's upstream flood +
 *      heartbeats reload the master's 600 budget; the master's chanmap+cfea
 *      reload the slave's HOLD);
 *   7. slave silence drops the master back to PROBING at exactly 600 slots.
 *
 * This also pins the master's JOIN matcher and the slave's cold-connect
 * builder to stay mutually compatible. */
#include "reac_master.h"
#include "reac_slave.h"
#include "reac_ctrl.h"
#include "reac_fsm.h"
#include "reac_tx.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static const uint8_t M_SRC[6]  = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
static const uint8_t S_SRC[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
static const uint8_t BCAST[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define FPS 8000

struct court {
	struct reac_master m;
	struct reac_slave  s;
	uint16_t m_counter_next;   /* free-run oracle for the master counter */
	/* tallies */
	long m_probes, m_subs, m_announces, m_grants, m_chanmaps;
	long s_joins_fed, s_unicasts_fed, s_heartbeats_fed, s_floods_fed;
	int  m_granted_before_join;    /* the #130 regression flag */
	int  slave_on;                 /* feed slave frames into the master? */
};

/* One master slot: emit, feed the slave (if PHY up), feed the slave's response
 * back into the master. Returns 0, or -1 on an internal check failure. */
static int step(struct court *c)
{
	uint8_t mf[REAC_FRAME_BYTES];
	uint16_t counter;
	int idx;

	/* --- the master's slot emission -------------------------------------- */
	enum reac_master_emit e = reac_master_next(&c->m, &counter, &idx);
	if (counter != c->m_counter_next)
		return -1;                          /* counter must free-run */
	c->m_counter_next++;

	reac_tx_build(mf, NULL, 0, REAC_SAMPLES_PER_PKT, counter, M_SRC);
	reac_master_stamp(&c->m, mf, e, idx);

	switch (e) {
	case REAC_M_EMIT_PROBE:    c->m_probes++;    break;
	case REAC_M_EMIT_SUB01:    c->m_subs++;      break;
	case REAC_M_EMIT_SUB02:    c->m_subs++;      break;
	case REAC_M_EMIT_ANNOUNCE: c->m_announces++; break;
	case REAC_M_EMIT_GRANT:
		c->m_grants++;
		if (c->s_joins_fed == 0)
			c->m_granted_before_join = 1;   /* the defect this task kills */
		break;
	case REAC_M_EMIT_CHANMAP:  c->m_chanmaps++;  break;
	case REAC_M_EMIT_FILLER:   break;
	}

	if (!c->slave_on)
		return 0;

	/* --- the slave sees the master frame (frame-arrival = its clock) ----- */
	struct reac_ctrl_parsed pm;
	reac_ctrl_parse(mf, REAC_FRAME_BYTES, &pm);
	struct reac_slave_decision d = reac_slave_step_rx(&c->s, &pm);

	/* --- the slave's response, via the SAME builders the live engine uses,
	 * classified + ingested by the master exactly like the pacer drain ---- */
	uint8_t sf[2048];
	size_t n = 0;
	uint16_t sc = c->s.fsm.counter;

	/* ONE frame per slot — control REPLACES the audio/flood frame, never adds a
	 * second (#130): with_join / with_heartbeat SELECT which frame this slot is. */
	switch (d.emit) {
	case REAC_SLAVE_EMIT_NONE:
		return 0;
	case REAC_SLAVE_EMIT_FLOOD_FILLER:
		/* the bounded broadcast presence-flood (zero control block + live audio) */
		n = reac_ctrl_build_flood_filler(sf, BCAST, S_SRC, sc, 16, NULL,
		                                 REAC_SAMPLES_PER_PKT);
		c->s_floods_fed++;
		break;
	case REAC_SLAVE_EMIT_COLDCONNECT:
		/* the unicast cold-connect phase: cdea 04 03 on the grid, audio between */
		if (d.with_join) {
			n = reac_ctrl_build_coldconnect(sf, c->s.fsm.master_mac, S_SRC, sc,
			                                16, NULL, REAC_SAMPLES_PER_PKT);
			c->s_joins_fed++;
		} else {
			n = reac_ctrl_build_upstream_filler(sf, c->s.fsm.master_mac, S_SRC, sc,
			                                    16, NULL, REAC_SAMPLES_PER_PKT);
			c->s_unicasts_fed++;
		}
		break;
	case REAC_SLAVE_EMIT_UPSTREAM_AUDIO:
		/* the ~1/s keep-alive REPLACES the audio frame on the slot the FSM flags */
		if (d.with_heartbeat) {
			n = reac_ctrl_build_box_hb(sf, c->s.fsm.master_mac, S_SRC, sc);
			c->s_heartbeats_fed++;
		} else {
			n = reac_ctrl_build_upstream_filler(sf, c->s.fsm.master_mac, S_SRC, sc,
			                                    16, NULL, REAC_SAMPLES_PER_PKT);
			c->s_unicasts_fed++;
		}
		break;
	case REAC_SLAVE_EMIT_HEARTBEAT:
		n = reac_ctrl_build_box_hb(sf, c->s.fsm.master_mac, S_SRC, sc);
		c->s_heartbeats_fed++;
		break;
	}
	if (n > 0) {
		struct reac_ctrl_parsed ps;
		enum reac_master_rx_event ev;
		if (reac_ctrl_classify_box_frame(sf, n, M_SRC, &ps, &ev) == 0)
			reac_master_rx(&c->m, ev, ps.src, sf + 18);
	}
	return 0;
}

int main(void)
{
	struct court c;
	memset(&c, 0, sizeof c);
	reac_master_init(&c.m, M_SRC, NULL, FPS);   /* S-1608 downstream (default) */
	struct reac_slave_cfg scfg = { .ifname = NULL, .box_channels = 16,
	                               .sample_rate = 96000, .src_mac = S_SRC };
	reac_slave_fsm_init(&c.s, &scfg);

	/* 1. the master probes first: 2 s alone on the wire — probes + announces +
	 * the sub-state-0x03 chanmap walk (§4: the box's parser needs it to
	 * recognize a master), NO grant, state stays PROBING (nothing to court). */
	c.slave_on = 0;
	for (long i = 0; i < 2L * FPS; i++)
		CHK(step(&c) == 0);
	CHK(c.m.state == REAC_M_PROBING);
	CHK(c.m_probes > 0 && c.m_announces > 0);
	CHK(c.m_grants == 0);          /* no grant until the box's cold-connect JOIN */
	CHK(c.m_chanmaps > 0);         /* §4: chanmap advertised while unlinked too */
	CHK(c.m_subs > 0);             /* the sub01/sub02 keepalives flow too */

	/* 2. the box PHY comes up: it floods + JOINs; the master grants ONLY
	 * after the cold-connect, both sides walk the golden ordering. */
	struct reac_slave_decision d0 = reac_slave_step_phy(&c.s, 1);
	CHK(d0.state == FSM_FLOOD_ANNOUNCE);
	c.slave_on = 1;

	long slot_master_established = -1, slot_slave_established = -1;
	for (long i = 0; i < 5L * FPS; i++) {
		CHK(step(&c) == 0);
		if (slot_slave_established < 0 && c.s.fsm.state == FSM_ESTABLISHED)
			slot_slave_established = i;
		if (slot_master_established < 0 && c.m.state == REAC_M_ESTABLISHED)
			slot_master_established = i;
		if (slot_master_established >= 0 && slot_slave_established >= 0)
			break;
	}
	CHK(!c.m_granted_before_join);            /* grant ONLY after the JOIN */
	CHK(c.s_joins_fed > 0 && c.m_grants > 0);
	CHK(slot_slave_established >= 0);         /* box linked off the echoed grant */
	CHK(slot_master_established >= 0);        /* we linked off its first unicast */
	CHK(slot_slave_established <= slot_master_established);
	CHK(memcmp(c.m.box_mac, S_SRC, 6) == 0);  /* the box we latched */
	CHK(memcmp(c.s.fsm.master_mac, M_SRC, 6) == 0);   /* the master it learned */
	CHK(memcmp(c.m.join_blk, "\x04\x03", 2) == 0);    /* echoing its block */

	/* 3. steady state holds >= 5 simulated seconds: the slave's upstream
	 * flood + heartbeats hold our 600 budget; our chanmap+cfea hold its HOLD.
	 * Both control streams run ~1/s each. */
	long cm0 = c.m_chanmaps, an0 = c.m_announces, hb0 = c.s_heartbeats_fed;
	for (long i = 0; i < 5L * FPS; i++) {
		CHK(step(&c) == 0);
		CHK(c.m.state == REAC_M_ESTABLISHED);
		CHK(c.s.fsm.state == FSM_ESTABLISHED);
	}
	CHK(c.m_chanmaps - cm0 >= 4 && c.m_announces - an0 >= 4);   /* ~1/s each */
	CHK(c.s_heartbeats_fed - hb0 >= 4);       /* the box keep-alive flows */

	/* 4. the box goes silent: the master holds for exactly its 600-frame
	 * budget, then drops back to PROBING (peer-gone) — and keeps counting. */
	c.slave_on = 0;
	for (int i = 0; i < REAC_M_LINKCHECK_RELOAD - 1; i++)
		CHK(step(&c) == 0);
	CHK(c.m.state == REAC_M_ESTABLISHED);     /* 599 silent slots: still held */
	CHK(step(&c) == 0);
	CHK(c.m.state == REAC_M_PROBING);         /* the 600th drains the budget */
	CHK(c.m.drop_reason == REAC_M_DROP_PEER_GONE);

	printf("OK: full offline courtship — master probes first, grants only on the "
	       "box's cold-connect (echoed), box links off the grant, master links off "
	       "the box's first unicast, 5 s steady HOLD both ways, peer-gone at "
	       "exactly %d silent slots\n", REAC_M_LINKCHECK_RELOAD);
	return 0;
}
