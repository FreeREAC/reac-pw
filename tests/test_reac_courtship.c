// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The full pure-FSM courtship: OUR master against OUR slave, no sockets.
 *
 * The strongest offline gate for the event-driven establishment (#130): both
 * halves are pure decision cores, so the whole M-5000 <-> S-1608 sequence runs
 * as a frame-by-frame simulation. Every master emission (reac_master_next +
 * reac_master_stamp onto a reac_downstream_build FILLER) is parsed and fed into
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
#include <reac/reac_ports.h>
#include <reac/reac_master.h>
#include <reac/transport/reac_slave.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_fsm.h>
#include <reac/transport/reac_role_swap.h>   /* the role lifecycle answers off THESE FSMs */
#include <reac/transport/reac_segment_ident.h> /* W1: the SEGMENT's answer, off the same FSMs */
#include <reac/transport/reac_tx.h>
#include <reac/reac.h>
#include <reac/reac_encode.h>

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
	long m_probes, m_subs, m_announces, m_grants, m_chanmaps, m_enrolls;
	long s_joins_fed, s_unicasts_fed, s_heartbeats_fed, s_floods_fed;
	long s_configs_fed;            /* box config-announces the master RECOGNIZED */
	/* Master downstream frames the SLAVE decoded — the same thing reac_rx's
	 * frames_ok counts on a real wire under REAC_RX_ACCEPT_DOWNSTREAM, and the
	 * evidence the segment's heard-latch is stepped from (reac_segment_ident.h). */
	uint64_t s_downstream_seen;
	unsigned cc_phase;             /* the box's cold-connect frame rotation */
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

	reac_downstream_build(mf, NULL, 0, REAC_SAMPLES_PER_PKT, counter, M_SRC);
	reac_master_stamp(&c->m, mf, e, idx);

	switch (e) {
	case REAC_M_EMIT_SCENE_CHUNK: c->m_probes++;  break;
	case REAC_M_EMIT_SCENE_HEAD:  c->m_subs++;    break;
	case REAC_M_EMIT_SCENE_TAIL:  c->m_subs++;    break;
	case REAC_M_EMIT_ANNOUNCE: c->m_announces++; break;
	case REAC_M_EMIT_GRANT:
		c->m_grants++;
		if (c->s_joins_fed == 0)
			c->m_granted_before_join = 1;   /* the defect this task kills */
		break;
	case REAC_M_EMIT_CHANMAP:  c->m_chanmaps++;  break;
	case REAC_M_EMIT_ENROLL:   c->m_enrolls++;   break;
	case REAC_M_EMIT_FILLER:   break;
	}

	if (!c->slave_on)
		return 0;

	/* --- the slave sees the master frame (frame-arrival = its clock) ----- */
	c->s_downstream_seen++;
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
			/* A real box's cold-connect is a ROTATION, not one repeated frame
			 * (reac_slave.c's coldconnect_phase % 8): the 04 03 0014 join, the
			 * 0013/0016/001a inventory, and — the one that matters here — the
			 * config-announce in which it DECLARES WHAT IT IS. The master learns the
			 * box from that frame and from nothing else, so a harness that only ever
			 * replayed the join was modelling a box that never introduces itself. */
			switch (c->cc_phase++ % 8) {
			case 4:
				n = reac_ctrl_build_config_announce(sf, c->s.fsm.master_mac, S_SRC,
				                                    sc, 16);
				break;
			default:
				n = reac_ctrl_build_coldconnect(sf, c->s.fsm.master_mac, S_SRC, sc,
				                                16, NULL, REAC_SAMPLES_PER_PKT);
				c->s_joins_fed++;
				break;
			}
		} else {
			n = reac_ctrl_build_upstream_filler(sf, c->s.fsm.master_mac, S_SRC, sc,
			                                    16, NULL, REAC_SAMPLES_PER_PKT);
			c->s_unicasts_fed++;
		}
		break;
	case REAC_SLAVE_EMIT_UPSTREAM_AUDIO:
		/* the ~1/s keep-alive REPLACES the audio frame on the slot the FSM flags */
		if (d.with_heartbeat) {
			n = reac_ctrl_build_box_hb(sf, c->s.fsm.master_mac, S_SRC, sc, 16);
			c->s_heartbeats_fed++;
		} else {
			n = reac_ctrl_build_upstream_filler(sf, c->s.fsm.master_mac, S_SRC, sc,
			                                    16, NULL, REAC_SAMPLES_PER_PKT);
			c->s_unicasts_fed++;
		}
		break;
	case REAC_SLAVE_EMIT_HEARTBEAT:
		n = reac_ctrl_build_box_hb(sf, c->s.fsm.master_mac, S_SRC, sc, 16);
		c->s_heartbeats_fed++;
		break;
	}
	if (n > 0) {
		/* The master's REAL ingest order (reac_pacer_rx_ingest): RECOGNIZE first,
		 * then feed the FSM. Recognition is not decoration here — it is the only
		 * way the master ever learns the box's width, and without it there is
		 * nothing to enroll and no grant to emit. Modelling the courtship without
		 * it would be testing a master that cannot court anything. */
		const struct reac_box_model *bm = reac_ctrl_identify_box(sf, n);
		struct reac_box_ports cports;
		if (bm && reac_ports_parse(sf + REAC_CTRL_BLOCK_OFF, &cports) == 0) {
			c->s_configs_fed++;
			/* The base comes off the announce in the very frame we just built,
			 * exactly as reac_pacer_rx_ingest takes it — not from bm->in_ch. */
			reac_master_set_box(&c->m, bm->in_ch, bm->out_ch,
			                    cports.headamp_base);
		}
		struct reac_ctrl_parsed ps;
		enum reac_master_rx_event ev;
		if (reac_ctrl_classify_box_frame(sf, n, M_SRC, &ps, &ev) == 0)
			reac_master_rx(&c->m, ev, ps.src, sf + 18);
	}
	return 0;
}

/* THE OPERATOR'S CASE, DRIVEN BY THE REAL SLAVE FSM: asked to be the recorder
 * end on a wire with no desk on it. The engine is up and announcing — the
 * bounded broadcast FILLER flood is a box's presence announcement — and it never
 * links, because it has nothing to link to: reac_fsm hands off to the unicast
 * cold-connect only once the master MAC is LEARNED, and on a silent wire it
 * never is. So the honest terminal here is the flood, and the role answer must
 * SAY the hunt for the whole of it and never reach `applied` (arbitration §8; a
 * "recorder applied" lamp over a desk that is not there is the lie this pins
 * shut). The OTHER hunt — cold-connecting a master that has been heard but has
 * not granted — is walked with real golden frames in the establishment loop of
 * main() below. */
static int test_quiet_wire_recorder_never_leaves_the_hunt(void)
{
	struct reac_slave s;
	struct reac_slave_cfg scfg = { .ifname = NULL, .box_channels = 16,
	                               .sample_rate = 96000, .src_mac = S_SRC };
	reac_slave_fsm_init(&s, &scfg);

	struct reac_role_swap sw;
	reac_role_swap_init(&sw, REAC_ROLE_MASTER);      /* booted as the mixer end */
	CHK(reac_role_swap_request(&sw, REAC_ROLE_SLAVE) != 0);   /* the console asks */
	reac_role_swap_closed(&sw);                      /* the master engine goes down */
	CHK(strcmp(reac_role_swap_state(&sw, REAC_ROLE_ENGINE_DOWN),
	           REAC_ROLE_STATE_REESTABLISH_PENDING) == 0);
	reac_role_swap_opened(&sw, REAC_ROLE_SLAVE);     /* the slave engine comes up */

	struct reac_slave_decision d = reac_slave_step_phy(&s, 1);
	CHK(d.state == FSM_FLOOD_ANNOUNCE);

	/* PRESENCE BEFORE ABSENCE: the engine must actually be DOING something, or
	 * "never established" would be true of a slave that never started. */
	int flooded = 0;
	for (long i = 0; i < 10L * FPS; i++) {
		d = reac_slave_step_tick(&s);
		if (d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER) flooded++;
		CHK(d.state != FSM_ESTABLISHED);
		CHK(s.fsm.have_master == 0);   /* nothing to learn on a silent wire */
		CHK(strcmp(reac_role_swap_state(&sw,
		           reac_role_engine_of_slave(1, s.fsm.state == FSM_ESTABLISHED)),
		           REAC_ROLE_STATE_HUNTING) == 0);
	}
	CHK(flooded > 0);          /* it announced: a real, live hunt, not a dead engine */
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

	/* THE ROLE ANSWER, READ OFF THIS VERY FSM (arbitration §8). A master alone
	 * on the wire is PERFORMING its role — it holds the segment, it paces, it
	 * probes — so the mixer end reads `applied` with no box in sight. That
	 * asymmetry against the slave (below) is the whole reason the lifecycle
	 * core exists. */
	struct reac_role_swap sw_m;
	reac_role_swap_init(&sw_m, REAC_ROLE_MASTER);
	reac_role_swap_opened(&sw_m, REAC_ROLE_MASTER);
	CHK(strcmp(reac_role_swap_state(&sw_m,
	           reac_role_engine_of_master(1, c.m.state)),
	           REAC_ROLE_STATE_APPLIED) == 0);

	/* 2. the box PHY comes up: it floods + JOINs; the master grants ONLY
	 * after the cold-connect, both sides walk the golden ordering. */
	struct reac_slave_decision d0 = reac_slave_step_phy(&c.s, 1);
	CHK(d0.state == FSM_FLOOD_ANNOUNCE);
	c.slave_on = 1;

	/* The recorder end's own record, walked with the golden frames: `applied` is
	 * unreachable for it until the master has enrolled it, so every flood,
	 * cold-connect and TX-mute slot on the way there answers `role_hunting`. */
	struct reac_role_swap sw_join;
	reac_role_swap_init(&sw_join, REAC_ROLE_SLAVE);
	reac_role_swap_opened(&sw_join, REAC_ROLE_SLAVE);

	/* W1: THE SEGMENT'S OWN ANSWER, walked beside the role's. A recorder has no
	 * reac-playback node, so this is everything a console can read about it —
	 * and before the desk is heard, "everything" must be a set of honest
	 * absences, not defaults dressed as facts. The latch is stepped from the
	 * master frames this slave actually decodes, which is what reac_rx counts on
	 * a real wire. */
	struct reac_segment_heard heard;
	struct reac_segment_answer ans;
	reac_segment_heard_init(&heard, 0);
	reac_segment_answer_slave(&ans, heard.heard, 0, 96000, REAC_MAX_CHANNELS);
	CHK(strcmp(ans.master_state, "none") == 0);
	CHK(strcmp(ans.master_mac, "none") == 0);
	CHK(strcmp(ans.pace_source, "free-run") == 0);
	CHK(strcmp(ans.rival_kind, "none") == 0);

	long slot_master_established = -1, slot_slave_established = -1;
	for (long i = 0; i < 5L * FPS; i++) {
		CHK(step(&c) == 0);
		if (c.s.fsm.state != FSM_ESTABLISHED)
			CHK(strcmp(reac_role_swap_state(&sw_join,
			           reac_role_engine_of_slave(1, 0)),
			           REAC_ROLE_STATE_HUNTING) == 0);
		/* The aggregate follows the SAME wire, slot by slot: the desk is being
		 * heard from the first decoded downstream frame — before the grant, and
		 * before `applied` — because presence and enrolment are two facts. */
		reac_segment_heard_step(&heard, c.s_downstream_seen,
		                        REAC_SEGMENT_HEARD_QUIET_TICKS);
		CHK(heard.heard == 1);
		if (slot_slave_established < 0 && c.s.fsm.state == FSM_ESTABLISHED)
			slot_slave_established = i;
		if (slot_master_established < 0 && c.m.state == REAC_M_ESTABLISHED)
			slot_master_established = i;
		if (slot_master_established >= 0 && slot_slave_established >= 0)
			break;
	}
	CHK(!c.m_granted_before_join);            /* grant ONLY after the JOIN */
	CHK(c.s_joins_fed > 0 && c.m_grants > 0);
	CHK(c.m_enrolls > 0);                     /* the pre-grant ENROLL frame flowed */
	CHK(slot_slave_established >= 0);         /* box linked off the grant */
	CHK(slot_master_established >= 0);        /* master self-completed after the burst */
	/* Ordering is now timing-dependent, not a protocol invariant: the master
	 * self-completes on the burst timer (~burst_len*STRIDE slots) while the slave
	 * links off the grant + its TX_MUTE dwell — either may reach ESTABLISHED first.
	 * Both reaching it (above) is the invariant that matters. */
	CHK(memcmp(c.m.box_mac, S_SRC, 6) == 0);  /* the box we latched */
	CHK(memcmp(c.s.fsm.master_mac, M_SRC, 6) == 0);   /* the master it learned */
	CHK(memcmp(c.m.join_blk, "\x04\x03", 2) == 0);    /* captured its cold-connect block */

	/* Both ends, answered off the established FSMs: the slave reaches `applied`
	 * only HERE — enrolled by a master — which is the fact the quiet-wire case
	 * below proves it cannot reach on its own. */
	struct reac_role_swap sw_s;
	reac_role_swap_init(&sw_s, REAC_ROLE_SLAVE);
	reac_role_swap_opened(&sw_s, REAC_ROLE_SLAVE);
	CHK(strcmp(reac_role_swap_state(&sw_s,
	           reac_role_engine_of_slave(1, c.s.fsm.state == FSM_ESTABLISHED)),
	           REAC_ROLE_STATE_APPLIED) == 0);
	CHK(strcmp(reac_role_swap_state(&sw_m,
	           reac_role_engine_of_master(1, c.m.state)),
	           REAC_ROLE_STATE_APPLIED) == 0);

	/* W1, ESTABLISHED: the recorder's node now carries the whole segment. The
	 * MAC is the one the FSM LEARNED off the wire in this very simulation — not
	 * a constant restated — carried through the packed atomic the engine thread
	 * publishes it as, so a byte lost in that crossing would fail here. And the
	 * geometry: the frames this slave decoded are the 40-channel master
	 * downstream its gate accepts and nothing else, so the answer is `desk` by
	 * the master's own classifier. That is the value the console's role policy
	 * needs to join what it is joined to rather than report it as a rival. */
	reac_segment_answer_slave(&ans, heard.heard,
	                          reac_mac48_pack(c.s.fsm.master_mac), 96000,
	                          REAC_MAX_CHANNELS);
	CHK(strcmp(ans.master_state, "foreign") == 0);
	CHK(strcmp(ans.master_mac, "00:40:ab:00:00:01") == 0);
	CHK(memcmp(M_SRC, "\x00\x40\xab\x00\x00\x01", 6) == 0);  /* the MAC just named */
	CHK(strcmp(ans.pace_source, "foreign-master") == 0);
	CHK(strcmp(ans.rival_kind, "desk") == 0);
	CHK(strcmp(ans.refusal, "none") == 0);      /* a desk is JOINED, not refused */
	CHK(strcmp(ans.conflict, "0") == 0);        /* a slave masters nothing to dispute */
	CHK(strcmp(ans.rate, "96000") == 0);

	/* 3. steady state holds >= 5 simulated seconds: the slave's upstream
	 * flood + heartbeats hold our 600 budget; our chanmap+cfea hold its HOLD.
	 * cfea free-runs ~1/s; the chanmap advances ONE window per control cycle
	 * (fps*10778/4000 slots ≈ 2.69 s — the measured M-300 choreography). */
	long cm0 = c.m_chanmaps, an0 = c.m_announces, hb0 = c.s_heartbeats_fed;
	for (long i = 0; i < 5L * FPS; i++) {
		CHK(step(&c) == 0);
		CHK(c.m.state == REAC_M_ESTABLISHED);
		CHK(c.s.fsm.state == FSM_ESTABLISHED);
	}
	CHK(c.m_chanmaps - cm0 >= 1 && c.m_announces - an0 >= 4);   /* 1/cycle + ~1/s */
	CHK(c.s_heartbeats_fed - hb0 >= 4);       /* the box keep-alive flows */

	/* 4. the box goes silent: the master holds for exactly its peer-gone budget
	 * (the measured ~6.5 s M-200i hold, fps-scaled — NOT the old 600 constant),
	 * then drops back to PROBING (peer-gone) — and keeps counting. */
	c.slave_on = 0;
	for (int i = 0; i < c.m.link_check_reload - 1; i++)
		CHK(step(&c) == 0);
	CHK(c.m.state == REAC_M_ESTABLISHED);     /* budget-1 silent slots: still held */
	CHK(step(&c) == 0);
	CHK(c.m.state == REAC_M_PROBING);         /* the last frame drains the budget */
	CHK(c.m.drop_reason == REAC_M_DROP_PEER_GONE);

	/* W1, THE OTHER DIRECTION OF THE SAME EVIDENCE: the desk stops. The frame
	 * count is cumulative and cannot fall, so only the latch's decay stops the
	 * segment reporting a master that has gone — and until it does, the answer
	 * must not flip early either. Both halves are asserted, because a latch that
	 * cleared immediately would pass a test that only checked it eventually
	 * cleared. */
	uint64_t frozen = c.s_downstream_seen;
	/* The tick that observes the LAST frame to arrive — the desk was still
	 * talking through the steady-state and peer-gone loops above, so the latch
	 * catches up here and the decay is counted from this point, not from a stale
	 * reading taken before all of it. */
	CHK(reac_segment_heard_step(&heard, frozen,
	                            REAC_SEGMENT_HEARD_QUIET_TICKS) == 1);
	for (int i = 0; i < REAC_SEGMENT_HEARD_QUIET_TICKS - 1; i++)
		CHK(reac_segment_heard_step(&heard, frozen,
		                            REAC_SEGMENT_HEARD_QUIET_TICKS) == 1);
	CHK(reac_segment_heard_step(&heard, frozen,
	                            REAC_SEGMENT_HEARD_QUIET_TICKS) == 0);
	reac_segment_answer_slave(&ans, heard.heard,
	                          reac_mac48_pack(c.s.fsm.master_mac), 96000,
	                          REAC_MAX_CHANNELS);
	CHK(strcmp(ans.master_state, "none") == 0);
	CHK(strcmp(ans.rival_kind, "none") == 0);
	CHK(strcmp(ans.pace_source, "free-run") == 0);

	if (test_quiet_wire_recorder_never_leaves_the_hunt()) return 1;

	printf("OK: full offline courtship — master probes first, grants only on the "
	       "box's cold-connect (echoed), box links off the grant, master links off "
	       "the box's first unicast, 5 s steady HOLD both ways, peer-gone at "
	       "exactly %d silent slots; the role answer walks the same FSMs and a "
	       "recorder on a quiet wire stays in the hunt; the SEGMENT answers "
	       "beside it in both roles and stops naming a desk that has gone\n",
	       c.m.link_check_reload);
	return 0;
}
