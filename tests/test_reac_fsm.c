// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The JOIN gate + the HOLD half of the connection FSM, driven by synthesized
 * master events (the parser is covered by test_reac_ctrl; here we exercise the
 * state logic). HOLD asserts — re-arm on master heartbeat, peer-gone after the
 * 600-frame budget, MAC-change drop — mirror the real-capture behaviour. */
#include "reac_fsm.h"
#include <stdio.h>
#include <string.h>

static const uint8_t M[6]  = { 0x00, 0x40, 0xab, 0x01, 0x02, 0x03 };
static const uint8_t M2[6] = { 0x00, 0x40, 0xab, 0x09, 0x09, 0x09 };

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static struct reac_ctrl_parsed mk(enum reac_ctrl_kind k, const uint8_t *s)
{
	struct reac_ctrl_parsed p;
	memset(&p, 0, sizeof p);
	p.kind = k;
	memcpy(p.src, s, 6);
	return p;
}

static void establish(struct reac_fsm *fsm)
{
	struct reac_ctrl_parsed g = mk(REAC_CTRL_GRANT, M);
	reac_fsm_init(fsm);
	reac_fsm_step(fsm, FSM_EV_PHY_UP, NULL);
	reac_fsm_step(fsm, FSM_EV_RX, &g);            /* FLOOD: learn master -> COLDCONNECT */
	reac_fsm_step(fsm, FSM_EV_RX, &g);            /* COLDCONNECT grant -> open ACK window */
	int guard = 0;                                /* ACK window + dwell -> ESTABLISHED */
	while (fsm->state != FSM_ESTABLISHED &&
	       guard++ < REAC_FSM_GRANT_ACK_FRAMES + REAC_FSM_TXMUTE_DWELL + 100)
		reac_fsm_step(fsm, FSM_EV_TICK, NULL);
}

int main(void)
{
	struct reac_fsm fsm;
	struct reac_fsm_out o;

	reac_fsm_init(&fsm);
	CHK(fsm.state == FSM_PHY_DOWN);

	/* PHY up -> the BOUNDED broadcast presence-flood. It is broadcast-ONLY: no
	 * cold-connect rides alongside it (#130, byte-verified 2026-07-11). */
	o = reac_fsm_step(&fsm, FSM_EV_PHY_UP, NULL);
	CHK(o.state == FSM_FLOOD_ANNOUNCE && o.action == FSM_ACT_FLOOD_BCAST);
	CHK(!o.emit_join);
	o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.action == FSM_ACT_FLOOD_BCAST && !o.emit_join);

	/* learn the master from its L2 source (a probe): still flooding, still no join */
	struct reac_ctrl_parsed probe = mk(REAC_CTRL_PROBE, M);
	o = reac_fsm_step(&fsm, FSM_EV_RX, &probe);
	CHK(o.state == FSM_FLOOD_ANNOUNCE && o.action == FSM_ACT_FLOOD_BCAST && !o.emit_join);
	CHK(fsm.have_master && memcmp(fsm.master_mac, M, 6) == 0);

	/* flood the rest of the bounded burst: broadcast every step, never a join,
	 * until the burst is spent (master learned) -> hand off to FSM_COLDCONNECT. */
	while (fsm.state == FSM_FLOOD_ANNOUNCE) {
		o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
		CHK(o.action == FSM_ACT_FLOOD_BCAST && !o.emit_join);
	}
	CHK(fsm.state == FSM_COLDCONNECT && fsm.flood_frames >= REAC_FSM_FLOOD_BURST);

	/* the cold-connect phase: unicast to the master every step, the cdea 04 03 on
	 * the retry grid, unicast audio FILLER between — ONE frame per step either way.
	 * The first grid slot carries the cold-connect. */
	o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.state == FSM_COLDCONNECT && o.action == FSM_ACT_UNICAST_COLDCONNECT && o.emit_join);
	for (int i = 0; i < REAC_FSM_JOIN_RETRY_PERIOD - 1; i++) {
		o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
		CHK(o.action == FSM_ACT_UNICAST_COLDCONNECT && !o.emit_join);   /* audio filler */
	}
	o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.action == FSM_ACT_UNICAST_COLDCONNECT && o.emit_join);       /* next grid slot */

	/* the master echoes the box block back as the grant. A real box does NOT mute
	 * here — it replies with the 0016/001a inventory (post-grant ACK) while the
	 * master keeps probing, and only then does the desk stop hunting = LINKED. So
	 * the grant OPENS the ACK window: we stay in COLDCONNECT re-emitting the
	 * escalation, then fall to TX_MUTE once it elapses. */
	struct reac_ctrl_parsed g = mk(REAC_CTRL_GRANT, M);
	o = reac_fsm_step(&fsm, FSM_EV_RX, &g);
	CHK(o.state == FSM_COLDCONNECT && fsm.have_master && memcmp(fsm.master_mac, M, 6) == 0);
	CHK(o.action == FSM_ACT_UNICAST_COLDCONNECT);
	{
		int guard = 0;
		while (fsm.state == FSM_COLDCONNECT && guard++ < REAC_FSM_GRANT_ACK_FRAMES + 10)
			o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
		CHK(o.state == FSM_TX_MUTE && guard > 1000);   /* held for the ACK window */
	}

	for (int i = 0; i < REAC_FSM_TXMUTE_DWELL; i++)
		o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.state == FSM_ESTABLISHED && o.action == FSM_ACT_UNICAST_AUDIO);

	/* HOLD: established tick decrements the peer-alive countdown */
	int lc0 = fsm.link_check;
	o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(fsm.link_check == lc0 - 1 && o.action == FSM_ACT_UNICAST_AUDIO);

	/* a master heartbeat re-arms it to 600 */
	struct reac_ctrl_parsed hb = mk(REAC_CTRL_MASTER_HB, M);
	reac_fsm_step(&fsm, FSM_EV_RX, &hb);
	CHK(fsm.link_check == REAC_FSM_LINKCHECK_RELOAD);

	/* 600 silent ticks -> peer-gone DROP */
	for (int i = 0; i < REAC_FSM_LINKCHECK_RELOAD; i++)
		o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.state == FSM_DROP && fsm.drop_reason == FSM_DROP_PEER_GONE);

	/* MAC-change DROP */
	establish(&fsm);
	CHK(fsm.state == FSM_ESTABLISHED);
	struct reac_ctrl_parsed hb2 = mk(REAC_CTRL_MASTER_HB, M2);
	o = reac_fsm_step(&fsm, FSM_EV_RX, &hb2);
	CHK(o.state == FSM_DROP && fsm.drop_reason == FSM_DROP_MAC_CHANGE);

	/* heartbeat is emitted ~1/s in ESTABLISHED (master kept alive so no drop) */
	establish(&fsm);
	int hb_seen = 0;
	for (int i = 0; i < 9000; i++) {  /* > internal HEARTBEAT_PERIOD (8000) */
		reac_fsm_step(&fsm, FSM_EV_RX, &hb);          /* re-arm */
		o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
		if (o.emit_heartbeat) hb_seen = 1;
	}
	CHK(hb_seen);

	printf("OK: FSM JOIN gate + HOLD (re-arm / peer-gone / mac-change) + heartbeat\n");
	return 0;
}
