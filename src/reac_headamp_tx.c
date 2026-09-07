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
	t->replay_set_only = 0;   /* an establishment arms the COMPLETE scene */
	/* Remember the slots so the re-assert can re-arm them itself. This is also
	 * the re-assert's ENABLE: a table that has never been armed belongs to a
	 * master that has never established (or one running REACPW_NO_HEADAMP), and
	 * such a master must not start writing to a box on a timer. */
	t->scene_base = base;
	t->scene_width = width;
	t->resweep_wait = 0;
}

void reac_headamp_tx_set_resweep(struct reac_headamp_tx *t, uint32_t frames)
{
	t->resweep_period = frames;
	t->resweep_wait = 0;
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
			t->resweep_wait = 0;   /* the cadence counts SILENCE, and this is not */
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
		/* Walk to the next cell THIS replay carries. A complete scene carries
		 * every cell; a set-only re-assert skips the ones the operator never set,
		 * which is a bounded scan (width x NPARAMS, at most 120) and no
		 * allocation — it runs on the RT pacer thread. */
		int total = (int)t->replay_width * REAC_HEADAMP_NPARAMS;
		while (t->replay_idx < total) {
			int c = t->replay_base + t->replay_idx / REAC_HEADAMP_NPARAMS;
			int p = t->replay_idx % REAC_HEADAMP_NPARAMS;
			t->replay_idx++;
			if (t->replay_set_only &&
			    !(c < REAC_HEADAMP_MAX_CH && t->set[c][p]))
				continue;
			*ch = (uint8_t)c;
			*param = (uint8_t)p;
			*value = reac_headamp_tx_effective(t, (uint8_t)c, (uint8_t)p);
			if (t->replay_idx >= total)
				t->replay_width = 0;   /* scene complete — back to silence */
			else
				t->replay_wait = REAC_HEADAMP_SWEEP_STRIDE - 1;
			t->resweep_wait = 0;       /* the cadence counts SILENCE */
			return 1;
		}
		t->replay_width = 0;   /* a set-only sweep with nothing left to send */
	}

	/* PERIODIC RE-ASSERT OF THE SET CELLS. A real M-200 stays silent here —
	 * committed captures hold 20.8 s / 27.8 s / 33.0 s of established traffic
	 * with phantom lit and ZERO head-amp records (COLDCONNECT-clean-2026-07-24,
	 * BIDIR-reboot-2026-07-11, matrix-m200-s1608-2026-07-11) — but the protocol
	 * has no readback, so asserting once leaves us no way to notice, let alone
	 * correct, a box that has dropped a setting or missed the frame that carried
	 * it (measured on the live rig: 400 000 frames spanning a phantom write held
	 * three head-amp records and then nothing).
	 *
	 * DISABLED UNLESS A CALLER STATES A PERIOD, and the shipped
	 * REAC_HEADAMP_RESWEEP_SECONDS is 0: a refresh overrides a change made at the
	 * box's own panel within one cadence, and taking that authority over 48 V is
	 * the operator's decision to make once the capture gate has run.
	 *
	 * SET CELLS ONLY. An unset cell's enrolling default is phantom OFF, and
	 * re-sending it every couple of seconds would darken a channel the operator
	 * lit at the box itself; absence of a cell means we have no opinion, and no
	 * opinion is silence. The full argument, the named risk (a redundant op-0403
	 * is idempotent BY DEDUCTION, never measured) and the rig gate are in
	 * docs/HEADAMP-REASSERT-POLICY.md. */
	if (t->resweep_period && t->scene_width &&
	    ++t->resweep_wait >= t->resweep_period) {
		t->resweep_wait = 0;
		t->replay_base = t->scene_base;
		t->replay_width = t->scene_width;
		t->replay_idx = 0;
		t->replay_wait = 0;
		t->replay_set_only = 1;
	}
	return 0;
}
