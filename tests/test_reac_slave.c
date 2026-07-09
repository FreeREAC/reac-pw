// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* SLAVE-role establishment, driven from the gold §13d slave sequence. openmixer as
 * a SLAVE responds to an EXTERNAL master and LOCKS to its cadence; this asserts the
 * pure decision core (reac_slave_step_* over the reac_fsm brain) walks the captured
 * establishment + HOLD exactly:
 *
 *   §13d step 1  PHY up  -> we FLOOD broadcast FILLER (presence-flood) + emit JOIN
 *   §13d step 2  master cycles cdea 01 sub-states (PROBE)  -> we keep announcing
 *   §13d step 3  master GRANTS with a cdea 04 03 burst      -> we enter TX-mute
 *   §13d step 4  the instant the grant lands we STOP broadcasting (mute window)
 *   §13d step 5  TX-mute dwell elapses -> ESTABLISHED -> unicast OUR input channels
 *                upstream + a ~1/s box heartbeat; the master HB re-arms the loop-check
 *                (HOLD); peer-gone after the 600-frame budget drops us.
 *
 * The master frames are synthesized exactly as reac_ctrl_parse yields them from a
 * real master broadcast — the same control KINDS the captured M-5000 (reac-captures/
 * wired-reac-a-bothdirs, master 00:40:ab:ca:15:4d) emits: PROBE (cdea 01 cycling),
 * GRANT (cdea 04 03), MASTER_HB (cdea 01 03 0019). So the establishment is exercised
 * correct-by-construction against the capture-derived control plane, the only part
 * verifiable off a real desk. The real-link gate (we go establishing->established
 * against an external master + audio both ways) is the hardware-verify gate in
 * DESIGN.md; this test fixes everything below it. */
#include "reac_slave.h"
#include "reac_ctrl.h"
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* The captured master MAC (00:40:ab:ca:15:4d, the wired-reac reference desk). The
 * slave learns this from the L2 source, never from config. */
static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0xca, 0x15, 0x4d };
static const uint8_t MASTER2[6] = { 0x00, 0x40, 0xab, 0x09, 0x09, 0x09 };

/* Synthesize a parsed master frame of a given kind, as reac_ctrl_parse would
 * classify a real master broadcast (src = the master MAC, dst = broadcast). */
static struct reac_ctrl_parsed master_frame(enum reac_ctrl_kind k, const uint8_t *src)
{
	struct reac_ctrl_parsed p;
	memset(&p, 0, sizeof p);
	p.kind = k;
	memcpy(p.src, src, 6);
	memset(p.dst, 0xff, 6);
	p.is_broadcast = 1;
	return p;
}

int main(void)
{
	struct reac_slave_cfg cfg = { .ifname = "lo", .box_channels = 16,
	                              .sample_rate = 96000, .src_mac = NULL };
	struct reac_slave s;
	struct reac_slave_decision d;

	/* The decision core needs no socket; init the FSM half directly. */
	reac_slave_fsm_init(&s, &cfg);
	CHK(s.box_channels == 16);
	CHK(s.fsm.state == FSM_PHY_DOWN);
	CHK(!atomic_load(&s.established));

	/* §13d step 1: PHY up -> begin the presence-flood. */
	d = reac_slave_step_phy(&s, 1);
	CHK(d.state == FSM_FLOOD_ANNOUNCE);
	CHK(d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER);   /* announce by flooding FILLER */

	/* announcing ticks: we keep emitting the JOIN cold-connect burst (sub-cmd 04,
	 * the §13b trigger) while unlinked. */
	d = reac_slave_step_tick(&s);
	CHK(d.emit == REAC_SLAVE_EMIT_JOIN);

	/* §13d step 2: the master cycles cdea 01 sub-states (PROBE). We learn the master
	 * from the L2 source and keep announcing (no grant yet -> stay flooding). */
	struct reac_ctrl_parsed probe = master_frame(REAC_CTRL_PROBE, MASTER);
	d = reac_slave_step_rx(&s, &probe);
	CHK(s.fsm.have_master && memcmp(s.fsm.master_mac, MASTER, 6) == 0);  /* learned */
	CHK(d.state == FSM_FLOOD_ANNOUNCE);
	CHK(d.emit == REAC_SLAVE_EMIT_JOIN);            /* still announcing, not linked */
	CHK(!atomic_load(&s.established));

	/* a master heartbeat/announce before the grant is also just "still negotiating". */
	struct reac_ctrl_parsed ann = master_frame(REAC_CTRL_MASTER_ANNOUNCE, MASTER);
	d = reac_slave_step_rx(&s, &ann);
	CHK(d.state == FSM_FLOOD_ANNOUNCE);

	/* §13d step 3: the master GRANTS with a cdea 04 03 burst -> we accept + enter the
	 * TX-mute settle window. §13d step 4: we STOP broadcasting (emit nothing). */
	struct reac_ctrl_parsed grant = master_frame(REAC_CTRL_GRANT, MASTER);
	d = reac_slave_step_rx(&s, &grant);
	CHK(d.state == FSM_TX_MUTE);
	CHK(d.emit == REAC_SLAVE_EMIT_NONE);            /* the grant lands -> go silent */

	/* §13d step 5: the TX-mute dwell elapses -> ESTABLISHED. The dwell self-clocks on
	 * the master cadence (one tick per master frame interval); drive it with ticks. */
	for (int i = 0; i < REAC_FSM_TXMUTE_DWELL; i++)
		d = reac_slave_step_tick(&s);
	CHK(d.state == FSM_ESTABLISHED);
	CHK(d.emit == REAC_SLAVE_EMIT_UPSTREAM_AUDIO);  /* now we return our inputs up */
	CHK(atomic_load(&s.established));

	/* HOLD: an established tick decrements the peer-alive countdown; a master HB
	 * re-arms it to the 600-frame budget. */
	int lc0 = s.fsm.link_check;
	d = reac_slave_step_tick(&s);
	CHK(s.fsm.link_check == lc0 - 1);
	CHK(d.emit == REAC_SLAVE_EMIT_UPSTREAM_AUDIO);

	struct reac_ctrl_parsed hb = master_frame(REAC_CTRL_MASTER_HB, MASTER);
	d = reac_slave_step_rx(&s, &hb);
	CHK(s.fsm.link_check == REAC_FSM_LINKCHECK_RELOAD);   /* re-armed */
	CHK(d.emit == REAC_SLAVE_EMIT_UPSTREAM_AUDIO);

	/* the ~1/s box heartbeat is flagged in ESTABLISHED (a master HB re-arms each
	 * round so we never drop while checking the heartbeat cadence). */
	int hb_seen = 0;
	for (int i = 0; i < 9000; i++) {   /* > the FSM's internal HEARTBEAT_PERIOD (8000) */
		reac_slave_step_rx(&s, &hb);                  /* re-arm */
		d = reac_slave_step_tick(&s);
		if (d.emit == REAC_SLAVE_EMIT_UPSTREAM_AUDIO && d.with_heartbeat)
			hb_seen = 1;
	}
	CHK(hb_seen);

	/* peer-gone: with the master frames stopped, the loop-check drains and we tear
	 * down (the master owns the clock; no frames -> no ticks fed -> drop). Re-
	 * establish cleanly so link_check is exactly the 600-frame budget, then run that
	 * many silent ticks and catch the DROP on the draining tick. */
	reac_slave_fsm_init(&s, &cfg);
	reac_slave_step_phy(&s, 1);
	reac_slave_step_rx(&s, &grant);                      /* grant -> TX_MUTE */
	for (int i = 0; i < REAC_FSM_TXMUTE_DWELL; i++)
		reac_slave_step_tick(&s);
	CHK(s.fsm.state == FSM_ESTABLISHED && s.fsm.link_check == REAC_FSM_LINKCHECK_RELOAD);
	for (int i = 0; i < REAC_FSM_LINKCHECK_RELOAD; i++)
		d = reac_slave_step_tick(&s);   /* the 600th tick drains link_check -> DROP */
	CHK(d.state == FSM_DROP);
	CHK(d.emit == REAC_SLAVE_EMIT_NONE);
	CHK(!atomic_load(&s.established));

	/* MAC-change DROP: a DIFFERENT master appearing while established tears us down
	 * (a slave bonds to ONE master, learned from L2). Re-establish, then flip MAC. */
	reac_slave_fsm_init(&s, &cfg);
	reac_slave_step_phy(&s, 1);
	reac_slave_step_rx(&s, &grant);                       /* grant -> TX_MUTE */
	for (int i = 0; i < REAC_FSM_TXMUTE_DWELL; i++)
		reac_slave_step_tick(&s);
	CHK(s.fsm.state == FSM_ESTABLISHED);
	struct reac_ctrl_parsed hb2 = master_frame(REAC_CTRL_MASTER_HB, MASTER2);
	d = reac_slave_step_rx(&s, &hb2);
	CHK(d.state == FSM_DROP && s.fsm.drop_reason == FSM_DROP_MAC_CHANGE);

	/* PHY down at any point -> immediate drop to PHY_DOWN, stop. */
	reac_slave_fsm_init(&s, &cfg);
	reac_slave_step_phy(&s, 1);
	d = reac_slave_step_phy(&s, 0);
	CHK(d.state == FSM_PHY_DOWN && d.emit == REAC_SLAVE_EMIT_NONE);

	printf("OK: slave establishment §13d (flood -> probe -> grant -> mute -> established "
	       "unicast upstream + heartbeat) + HOLD (re-arm / peer-gone / mac-change / phy-down) "
	       "locked to the master cadence\n");
	return 0;
}
