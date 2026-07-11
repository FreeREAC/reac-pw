// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* SLAVE-role establishment, driven from the gold §13d slave sequence. openmixer as
 * a SLAVE responds to an EXTERNAL master and LOCKS to its cadence; this asserts the
 * pure decision core (reac_slave_step_* over the reac_fsm brain) walks the captured
 * establishment + HOLD exactly:
 *
 *   §13d step 1  PHY up  -> we FLOOD broadcast FILLER (BOUNDED presence-flood)
 *   §13d step 2  flood done + master learned -> we STOP broadcasting and UNICAST
 *                our cold-connect (cdea 04 03) on a ~100 ms grid, audio between
 *   §13d step 3  master GRANTS with a cdea 04 03 burst      -> we enter TX-mute
 *   §13d step 4  the instant the grant lands we STOP transmitting (mute window)
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

	/* §13d step 1 / §13p.3: PHY up -> begin the BOUNDED broadcast presence-flood.
	 * It is broadcast-ONLY — no cold-connect rides alongside it (#130, byte-verified
	 * 2026-07-11: the flood and the cold-connect are separate TX phases). */
	d = reac_slave_step_phy(&s, 1);
	CHK(d.state == FSM_FLOOD_ANNOUNCE);
	CHK(d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER);   /* announce by flooding FILLER */
	CHK(!d.with_join);                             /* broadcast-only */

	d = reac_slave_step_tick(&s);
	CHK(d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER && !d.with_join);

	/* §13d step 2: the master cycles cdea 01 sub-states (PROBE). We learn the master
	 * from the L2 source and keep flooding (no grant yet, flood not done). */
	struct reac_ctrl_parsed probe = master_frame(REAC_CTRL_PROBE, MASTER);
	d = reac_slave_step_rx(&s, &probe);
	CHK(s.fsm.have_master && memcmp(s.fsm.master_mac, MASTER, 6) == 0);  /* learned */
	CHK(d.state == FSM_FLOOD_ANNOUNCE);
	CHK(d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER && !d.with_join);
	CHK(!atomic_load(&s.established));

	/* a master announce before the flood is done is still "still negotiating". */
	struct reac_ctrl_parsed ann = master_frame(REAC_CTRL_MASTER_ANNOUNCE, MASTER);
	d = reac_slave_step_rx(&s, &ann);
	CHK(d.state == FSM_FLOOD_ANNOUNCE);
	CHK(d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER && !d.with_join);

	/* flood the rest of the bounded burst -> the box STOPS broadcasting and switches
	 * to the unicast cold-connect phase (the emit flips FLOOD_FILLER -> COLDCONNECT). */
	while (d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER)
		d = reac_slave_step_tick(&s);
	CHK(s.fsm.state == FSM_COLDCONNECT);
	CHK(d.emit == REAC_SLAVE_EMIT_COLDCONNECT && d.with_join);   /* first grid slot: cdea 04 03 */
	d = reac_slave_step_tick(&s);
	CHK(d.emit == REAC_SLAVE_EMIT_COLDCONNECT && !d.with_join);  /* unicast audio filler */

	/* §13d step 3: the master GRANTS with a cdea 04 03 burst -> we accept + enter the
	 * TX-mute settle window. §13d step 4: we STOP transmitting (emit nothing). */
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

	/* #130 regression (byte-verified 2026-07-11): establishment is TWO distinct TX
	 * phases, ONE frame per counter slot throughout. The old cut floods broadcast
	 * FOREVER with a unicast cold-connect riding alongside (two frames per slot).
	 * Assert the corrected model: (1) the broadcast presence-flood is BOUNDED and
	 * join-free (broadcast-only), then (2) the box stops broadcasting and enters a
	 * pure UNICAST cold-connect phase that emits the cdea 04 03 on a fixed
	 * REAC_FSM_JOIN_RETRY_PERIOD grid with unicast audio FILLER between — never a
	 * broadcast, never idle, never a per-slot cold-connect spam. */
	{
		reac_slave_fsm_init(&s, &cfg);
		struct reac_ctrl_parsed mprobe = master_frame(REAC_CTRL_PROBE, MASTER);

		/* phase 1: the BOUNDED broadcast flood — always FLOOD_FILLER, never a join
		 * alongside. Keep the master learned (periodic probe) so it can hand off. */
		d = reac_slave_step_phy(&s, 1);
		long flood_steps = 0;
		while (d.emit == REAC_SLAVE_EMIT_FLOOD_FILLER) {
			CHK(!d.with_join);                 /* broadcast-only, no cold-connect alongside */
			flood_steps++;
			d = (flood_steps % 512 == 0) ? reac_slave_step_rx(&s, &mprobe)
			                             : reac_slave_step_tick(&s);
		}
		/* the flood STOPPED and handed off — it did NOT flood forever (the #130 bug) */
		CHK(flood_steps >= REAC_FSM_FLOOD_BURST);
		CHK(s.fsm.state == FSM_COLDCONNECT);
		CHK(d.emit == REAC_SLAVE_EMIT_COLDCONNECT);

		/* phase 2: the pure UNICAST cold-connect phase — every slot COLDCONNECT, the
		 * cdea 04 03 on a fixed REAC_FSM_JOIN_RETRY_PERIOD grid, audio filler between,
		 * one frame per slot (with_join SELECTS this slot's frame, never adds a 2nd). */
		const int n_cycles = 3;
		const int soak = n_cycles * REAC_FSM_JOIN_RETRY_PERIOD + 5;
		int join_frames = 0, grid_slots = 0, last_start = -1;
		for (int i = 0; i < soak; i++) {
			CHK(d.state == FSM_COLDCONNECT);
			CHK(d.emit == REAC_SLAVE_EMIT_COLDCONNECT);   /* unicast — never broadcast/idle */
			if (d.with_join) {
				join_frames++;
				grid_slots++;
				if (last_start >= 0)
					CHK(i - last_start == REAC_FSM_JOIN_RETRY_PERIOD);  /* fixed grid */
				last_start = i;
			}
			d = reac_slave_step_tick(&s);
		}
		CHK(grid_slots == n_cycles + 1);   /* the entry cold-connect + n_cycles grid slots */
		/* the audio filler dominates: cold-connects are a small minority of slots,
		 * not a per-slot spam (the literal #130 bug). */
		CHK(join_frames * 4 < soak);
	}

	printf("OK: slave establishment §13d (bounded flood -> cold-connect phase -> grant "
	       "-> mute -> established unicast upstream + heartbeat) + HOLD (re-arm / peer-gone "
	       "/ mac-change / phy-down) locked to the master cadence; #130: bounded broadcast "
	       "flood then a pure unicast cold-connect grid, ONE frame per counter slot\n");
	return 0;
}
