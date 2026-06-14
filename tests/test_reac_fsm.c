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
	reac_fsm_step(fsm, FSM_EV_RX, &g);            /* grant -> TX_MUTE */
	for (int i = 0; i < REAC_FSM_TXMUTE_DWELL; i++)
		reac_fsm_step(fsm, FSM_EV_TICK, NULL);    /* dwell -> ESTABLISHED */
}

int main(void)
{
	struct reac_fsm fsm;
	struct reac_fsm_out o;

	reac_fsm_init(&fsm);
	CHK(fsm.state == FSM_PHY_DOWN);

	o = reac_fsm_step(&fsm, FSM_EV_PHY_UP, NULL);
	CHK(o.state == FSM_FLOOD_ANNOUNCE && o.action == FSM_ACT_FLOOD_BCAST);
	o = reac_fsm_step(&fsm, FSM_EV_TICK, NULL);
	CHK(o.action == FSM_ACT_EMIT_JOIN);

	struct reac_ctrl_parsed g = mk(REAC_CTRL_GRANT, M);
	o = reac_fsm_step(&fsm, FSM_EV_RX, &g);
	CHK(o.state == FSM_TX_MUTE && fsm.have_master && memcmp(fsm.master_mac, M, 6) == 0);

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
