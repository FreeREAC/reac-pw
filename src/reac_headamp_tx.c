// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_headamp_tx.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */

#include <string.h>

void reac_headamp_tx_init(struct reac_headamp_tx *t)
{
	memset(t, 0, sizeof *t);
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

/* The enrolling defaults. phantom and pad stay safe-off: +48V into a ribbon mic
 * or an unbalanced line source can destroy it, and a box we have just enrolled
 * is by definition a box whose patch we do not yet know — the one direction that
 * is never recoverable is the one we must not take by default (a hot source
 * clipping, pad's business, is recoverable). SENS is deliberately NON-ZERO: the
 * box only ENROLS a channel whose arming scene carries a REAL (non-zero)
 * head-amp value; a channel armed all-zero is never enrolled, so no later
 * op-0403 write — however byte-perfect — ever commits it. Operator-confirmed on
 * the S-0808 (2026-07-23): a real-valued arming scene lit BOTH condensers, the
 * all-zero scene stayed dead. 0x20 = -42 dBu pad-off (reac_headamp_sens_db) —
 * moderate gain, and a value the M-200 desk itself arms (its input 1). phantom
 * stays OFF, so a non-zero SENS cannot drive 48V. */
#define REAC_HEADAMP_DEFAULT_PHANTOM 0x00
#define REAC_HEADAMP_DEFAULT_PAD     0x00
#define REAC_HEADAMP_DEFAULT_SENS    0x20

uint8_t reac_headamp_default(uint8_t param)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM: return REAC_HEADAMP_DEFAULT_PHANTOM;
	case REAC_HEADAMP_PAD:     return REAC_HEADAMP_DEFAULT_PAD;
	case REAC_HEADAMP_SENS:    return REAC_HEADAMP_DEFAULT_SENS;
	default:                   return 0;
	}
}

uint8_t reac_headamp_tx_effective(const struct reac_headamp_tx *t,
                                  uint8_t ch, uint8_t param)
{
	if (param >= REAC_HEADAMP_NPARAMS)
		return 0;
	/* The table is the operator's/config's intent; `set` is what distinguishes a
	 * deliberate 0 from an unset cell, so it — not the value — is the test. */
	if (t && ch < REAC_HEADAMP_MAX_CH && t->set[ch][param])
		return t->value[ch][param];
	return reac_headamp_default(param);
}

void reac_headamp_tx_arm_scene(struct reac_headamp_tx *t, uint8_t base,
                               uint8_t width)
{
	if (width == 0)
		return;
	if ((int)base + (int)width > REAC_HEADAMP_MAX_CH)
		width = (uint8_t)(REAC_HEADAMP_MAX_CH - base);
	t->replay_base = base;
	t->replay_width = width;
	t->replay_idx = 0;
	t->replay_wait = 0;   /* the first record goes on the next eligible slot */
}

#define REAC_HEADAMP_NCELLS (REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS)

int reac_headamp_tx_next(struct reac_headamp_tx *t, uint8_t *ch, uint8_t *param,
                         uint8_t *value)
{
	if (!t->active && t->replay_width == 0)
		return 0;

	/* Edge first: the newest change goes out on the next slot, ahead of an
	 * in-progress scene replay (an operator hitting phantom-off must reach the
	 * box promptly). One dirty cell per call. A cell edged mid-replay is emitted
	 * twice with the same absolute value — harmless: a lone op-0403 rewrite
	 * self-commits (HEADAMP-PROTOCOL-AUDIT-2026-07-22). */
	for (int idx = 0; idx < REAC_HEADAMP_NCELLS; idx++) {
		int c = idx / REAC_HEADAMP_NPARAMS, p = idx % REAC_HEADAMP_NPARAMS;
		if (t->dirty[c][p]) {
			t->dirty[c][p] = 0;
			*ch = (uint8_t)c;
			*param = (uint8_t)p;
			*value = t->value[c][p];
			return 1;
		}
	}

	/* Pending complete-scene replay: per-channel param triples in order, one
	 * record per REAC_HEADAMP_SWEEP_STRIDE frames, operator values where set and
	 * the enrolling defaults everywhere else — the same scene the grant sweep
	 * arms, re-put on the wire because THIS establishment may be a power-cycled
	 * box that came back blank (measured: m200-s1608-BIDIR-reboot — the M-200
	 * replays the whole width x 3 scene 10 ms behind every grant). */
	if (t->replay_width) {
		if (t->replay_wait > 0) {
			t->replay_wait--;
			return 0;
		}
		int c = t->replay_base + t->replay_idx / REAC_HEADAMP_NPARAMS;
		int p = t->replay_idx % REAC_HEADAMP_NPARAMS;
		*ch = (uint8_t)c;
		*param = (uint8_t)p;
		*value = reac_headamp_tx_effective(t, (uint8_t)c, (uint8_t)p);
		if (++t->replay_idx >= (int)t->replay_width * REAC_HEADAMP_NPARAMS)
			t->replay_width = 0;   /* scene complete — back to silence */
		else
			t->replay_wait = REAC_HEADAMP_SWEEP_STRIDE - 1;
		return 1;
	}

	/* NO periodic re-assert. A real M-200 keeps head-amp silence once armed —
	 * committed captures hold 20.8 s / 27.8 s / 33.0 s of established traffic
	 * with phantom lit and ZERO head-amp records (COLDCONNECT-clean-2026-07-24,
	 * BIDIR-reboot-2026-07-11, matrix-m200-s1608-2026-07-11). The 1 Hz frame is
	 * the op-0103 CHANMAP heartbeat and carries no head-amp cell. The box HOLDS
	 * its committed state while powered; a power-cycle is answered by the scene
	 * replay above, never by a timer. */
	return 0;
}
