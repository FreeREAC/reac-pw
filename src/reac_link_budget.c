// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_link_budget.h"
#include "reac_facts_pw.h"   /* REAC_SAMPLES_PER_PKT */

uint64_t reac_link_cost_kbit(unsigned pps, unsigned frame_bytes)
{
	if (pps == 0 || frame_bytes == 0)
		return 0;
	uint64_t bits = (uint64_t)pps *
	                ((uint64_t)frame_bytes + REAC_LINK_WIRE_OVERHEAD_BYTES) * 8u;
	return bits / 1000u;
}

unsigned reac_link_master_pps(unsigned sample_rate)
{
	return sample_rate / (unsigned)REAC_SAMPLES_PER_PKT;
}

int reac_link_budget_fits(unsigned link_mbit, uint64_t used_kbit, uint64_t want_kbit)
{
	if (link_mbit == 0)
		return 1;                       /* unknown link: see the header */
	uint64_t cap = (uint64_t)link_mbit * 1000u * REAC_LINK_BUDGET_NUM / REAC_LINK_BUDGET_DEN;
	return used_kbit + want_kbit <= cap;
}
