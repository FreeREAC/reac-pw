// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE DESK, 2026-09-16, IS THE FIXTURE. `enp131s0` is 100 Mbit/s full duplex and
 * carried four declared master segments at 96 kHz. The arms below are that port's
 * numbers, and the one that matters is the second master: it must NOT fit. */
#include "reac_link_budget.h"

#include <reac/reac.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#include <stdio.h>

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
	/* The geometry, from the protocol: 12 samples a frame at every rate. */
	CHK(reac_link_master_pps(REAC_SAMPLE_RATE_96K) == REAC_PKT_RATE_96K);
	CHK(reac_link_master_pps(REAC_SAMPLE_RATE_48K) == REAC_PKT_RATE_48K);
	CHK(reac_link_master_pps(REAC_SAMPLE_RATE_44K1) == REAC_PKT_RATE_44K1);
	CHK(reac_link_master_pps(0) == 0);

	/* ONE 96 kHz MASTER IS ~97 Mbit/s: 8000 x (1492 + 24) x 8. Measured against the
	 * desk's own qdisc, which passed 8238 pkt/s and reported the link at 99.6 Mbit/s. */
	uint64_t one = reac_link_cost_kbit(reac_link_master_pps(REAC_SAMPLE_RATE_96K), REAC_FRAME_BYTES);
	CHK(one == (uint64_t)REAC_PKT_RATE_96K * (REAC_FRAME_BYTES + REAC_LINK_WIRE_OVERHEAD_BYTES) * 8 / 1000);
	CHK(reac_link_cost_kbit(0, REAC_FRAME_BYTES) == 0);
	CHK(reac_link_cost_kbit(REAC_PKT_RATE_96K, 0) == 0);

	/* THE ANSWER THE DESK NEEDED. One master fits a 100 Mbit/s port; a second does
	 * not, and neither does a third or a fourth — which is what was running, at 387
	 * Mbit/s offered onto a 100 Mbit/s wire, with the etf qdisc discarding 75% of
	 * every segment's frames and nothing saying so. */
	CHK(reac_link_budget_fits(100, 0, one) == 1);
	CHK(reac_link_budget_fits(100, one, one) == 0);
	CHK(reac_link_budget_fits(100, 3 * one, one) == 0);

	/* AT 48 kHz TWO FIT AND FOUR DO NOT — the operator's own test, and why dropping the
	 * box to 48 k did not fix the silence: four segments at 48 k still offer 194 Mbit/s
	 * onto a 100 Mbit/s port. */
	uint64_t half = reac_link_cost_kbit(reac_link_master_pps(REAC_SAMPLE_RATE_48K), REAC_FRAME_BYTES);
	CHK(half == (uint64_t)REAC_PKT_RATE_48K * (REAC_FRAME_BYTES + REAC_LINK_WIRE_OVERHEAD_BYTES) * 8 / 1000);
	CHK(reac_link_budget_fits(100, half, half) == 1);
	CHK(reac_link_budget_fits(100, 2 * half, half) == 0);

	/* A GIGABIT PORT CARRIES TEN 96 kHz MASTERS AND REFUSES THE ELEVENTH: 10 x 97.024 =
	 * 970.2 Mbit/s fits, 11 do not. The trunk case, and the reason this is a budget
	 * rather than a one-master-per-port rule. */
	CHK(reac_link_budget_fits(1000, 9 * one, one) == 1);
	CHK(reac_link_budget_fits(1000, 10 * one, one) == 0);

	/* AN UNKNOWN LINK IS NOT A FULL ONE. `/sys/class/net/<dev>/speed` is unreadable on
	 * a veth, in a netns and on a down port; refusing there would turn every bench into
	 * a silent segment for want of a number. */
	CHK(reac_link_budget_fits(0, 100 * one, one) == 1);

	if (!fails)
		printf("reac_link_budget: ok — one 96 kHz master fits 100 Mbit/s, a second does not\n");
	return fails ? 1 : 0;
}
