// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_headamp_tx.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */

#include <string.h>

void reac_headamp_tx_init(struct reac_headamp_tx *t, int fps)
{
	memset(t, 0, sizeof *t);
	int period = fps * REAC_HEADAMP_REASSERT_NUM / REAC_HEADAMP_REASSERT_DEN;
	t->reassert_period = period > 1 ? period : 1;
	t->reassert_tick = t->reassert_period;
}

static int param_value_ok(uint8_t param, uint8_t value)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		return value <= 0x01;
	case REAC_HEADAMP_SENS:
		return value <= REAC_HEADAMP_SENS_MAX;
	default:
		return 0;
	}
}

int reac_headamp_tx_set(struct reac_headamp_tx *t, uint8_t ch, uint8_t param,
                        uint8_t value)
{
	if (ch >= REAC_HEADAMP_MAX_CH || !param_value_ok(param, value))
		return -1;
	t->value[ch][param] = value;
	t->set[ch][param] = 1;
	t->dirty[ch][param] = 1;   /* emit the change on the next eligible slot */
	t->active = 1;
	return 0;
}

/* Emit the cell at flattened index `idx` (idx = ch*NPARAMS + param). */
static void emit_at(const struct reac_headamp_tx *t, int idx,
                    uint8_t *ch, uint8_t *param, uint8_t *value)
{
	int c = idx / REAC_HEADAMP_NPARAMS;
	int p = idx % REAC_HEADAMP_NPARAMS;
	*ch = (uint8_t)c;
	*param = (uint8_t)p;
	*value = t->value[c][p];
}

#define REAC_HEADAMP_NCELLS (REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS)

int reac_headamp_tx_next(struct reac_headamp_tx *t, uint8_t *ch, uint8_t *param,
                         uint8_t *value)
{
	if (!t->active)
		return 0;

	/* Edge first: the newest change goes out on the next slot, ahead of any
	 * in-progress re-assert sweep (an operator hitting phantom-off must reach the
	 * box promptly, not wait a whole period). One dirty cell per call. */
	for (int idx = 0; idx < REAC_HEADAMP_NCELLS; idx++) {
		int c = idx / REAC_HEADAMP_NPARAMS, p = idx % REAC_HEADAMP_NPARAMS;
		if (t->dirty[c][p]) {
			t->dirty[c][p] = 0;
			emit_at(t, idx, ch, param, value);
			return 1;
		}
	}

	/* Periodic full re-assert (DMX): once per reassert_period, sweep every SET
	 * cell, one record per SWEEP_STRIDE frames, re-broadcasting the absolute
	 * values so a dropped record can never persist as stale box state. */
	if (t->sweeping) {
		if (--t->sweep_wait > 0)
			return 0;
		t->sweep_wait = REAC_HEADAMP_SWEEP_STRIDE;
		while (t->sweep_idx < REAC_HEADAMP_NCELLS) {
			int idx = t->sweep_idx++;
			int c = idx / REAC_HEADAMP_NPARAMS, p = idx % REAC_HEADAMP_NPARAMS;
			if (t->set[c][p]) {
				emit_at(t, idx, ch, param, value);
				return 1;
			}
		}
		t->sweeping = 0;   /* swept the whole table; wait for the next period */
		return 0;
	}

	if (--t->reassert_tick <= 0) {
		t->reassert_tick = t->reassert_period;
		t->sweeping = 1;
		t->sweep_idx = 0;
		t->sweep_wait = 1;   /* the first sweep record lands on the next call */
	}
	return 0;
}
