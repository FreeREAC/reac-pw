// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_headamp_state.h"

#include <stdio.h>
#include <string.h>

const char *reac_headamp_refuse_code(enum reac_headamp_refuse r)
{
	switch (r) {
	case REAC_HEADAMP_REFUSE_NO_BOX:       return "no-box";
	case REAC_HEADAMP_REFUSE_BOX_MASTER:   return "box-master";
	case REAC_HEADAMP_REFUSE_NO_BASE:      return "no-base";
	case REAC_HEADAMP_REFUSE_BAD_KEY:      return "bad-key";
	case REAC_HEADAMP_REFUSE_OUT_OF_RANGE: return "out-of-range";
	case REAC_HEADAMP_REFUSE_NONE:
	default:
		return "none";
	}
}

enum reac_headamp_refuse reac_headamp_cfg_decide(int box_master, int channels, int base)
{
	/* THE MORE SPECIFIC FACT FIRST. A box-master segment also has channels == 0
	 * (reac-pw publishes the master role's empties there rather than a capability
	 * the wire cannot carry), so testing `no-box` first would bury the one reason
	 * a surface can render as a sentence. */
	if (box_master)
		return REAC_HEADAMP_REFUSE_BOX_MASTER;
	if (channels <= 0)
		return REAC_HEADAMP_REFUSE_NO_BOX;
	if (base < 0)
		return REAC_HEADAMP_REFUSE_NO_BASE;
	return REAC_HEADAMP_REFUSE_NONE;
}

const char *reac_headamp_cfg_state(int box_master, int channels, int base)
{
	return reac_headamp_cfg_decide(box_master, channels, base) == REAC_HEADAMP_REFUSE_NONE
		? REAC_HEADAMP_STATE_APPLIED
		: REAC_HEADAMP_STATE_UNAVAILABLE;
}

void reac_headamp_asserted_init(struct reac_headamp_asserted *t)
{
	if (t)
		memset(t, 0, sizeof *t);
}

/* The same per-param range gate the send table applies, so a cell accepted here
 * is one reac_headamp_tx_set would accept too. */
static int value_in_range(uint8_t param, uint8_t value)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		return value <= 1;
	case REAC_HEADAMP_SENS:
		return value <= REAC_HEADAMP_SENS_MAX;
	default:
		return 0;
	}
}

int reac_headamp_asserted_set(struct reac_headamp_asserted *t, uint8_t ch,
                              uint8_t param, uint8_t value)
{
	if (!t || ch >= REAC_HEADAMP_MAX_CH || param >= REAC_HEADAMP_NPARAMS)
		return -1;
	if (!value_in_range(param, value))
		return -1;
	t->value[ch][param] = value;
	t->set[ch][param] = 1;
	return 0;
}

size_t reac_headamp_asserted_render(const struct reac_headamp_asserted *t,
                                    char *buf, size_t buflen)
{
	size_t used = 0;                  /* the snprintf return convention: what it WOULD take */
	if (buflen > 0)
		buf[0] = '\0';
	if (!t)
		return 0;

	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		for (int p = 0; p < REAC_HEADAMP_NPARAMS; p++) {
			if (!t->set[ch][p])
				continue;
			char cell[16];
			int n = snprintf(cell, sizeof cell, "%s%d:%d=%u",
			                 used ? "," : "", ch, p,
			                 (unsigned)t->value[ch][p]);
			if (n < 0)
				return used;
			/* Append only what fits, but keep counting so the caller can
			 * tell a truncation from a fit. */
			if (buflen > 0 && used < buflen - 1) {
				size_t room = buflen - 1 - used;
				size_t take = (size_t)n < room ? (size_t)n : room;
				memcpy(buf + used, cell, take);
				buf[used + take] = '\0';
			}
			used += (size_t)n;
		}
	}
	return used;
}
