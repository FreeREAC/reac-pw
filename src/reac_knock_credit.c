// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_knock_credit.h"

void reac_knock_credit_transfer(struct reac_master *m)
{
	uint16_t counter;
	int idx;
	if (m->state == REAC_M_IDLE)
		(void)reac_master_next(m, &counter, &idx);   /* IDLE -> PROBING, slot never sent */
	m->scene_inflight = 0;
	m->scene_complete = 1;
}
