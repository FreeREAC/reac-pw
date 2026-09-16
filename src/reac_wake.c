// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_wake — see reac_wake.h for the live failure, the firmware law behind it and the
// safety argument. This file is the ladder; docs/design/specs/
// 2026-09-16-a-dropped-box-wakes-on-a-phy-edge.md §3-§5 is what it instantiates.

#include "reac_wake.h"

#include <string.h>

void reac_wake_init(struct reac_wake *w, uint64_t now_ns)
{
	memset(w, 0, sizeof *w);
	w->opened_ns = now_ns;
}

void reac_wake_reopen(struct reac_wake *w, uint64_t now_ns)
{
	w->opened_ns = now_ns;
	w->refusal = REAC_WAKE_OK;
}

enum reac_wake_act reac_wake_step(struct reac_wake *w, uint64_t now_ns,
                                  const struct reac_wake_obs *o)
{
	(void)now_ns; (void)o;
	/* TODAY'S BEHAVIOUR, kept here only long enough for the test to fail against it:
	 * the master drives the wire and never touches its own link, whatever the wire
	 * says. 73 minutes of exactly this is the defect. */
	w->refusal = REAC_WAKE_PUSH_NOT_PROVEN;
	return REAC_WAKE_ACT_NONE;
}

const char *reac_wake_refusal_text(enum reac_wake_refusal r)
{
	switch (r) {
	case REAC_WAKE_OK:              return "nothing refused it";
	case REAC_WAKE_NOT_PROBING:     return "the master is not probing";
	case REAC_WAKE_NO_CARRIER:      return "there is no carrier to break";
	case REAC_WAKE_CARRIER_UNKNOWN: return "the carrier could not be read";
	case REAC_WAKE_BOX_IS_TALKING:  return "a box is transmitting";
	case REAC_WAKE_PUSH_NOT_PROVEN: return "our own scene push has not completed yet";
	case REAC_WAKE_SETTLING:        return "the last edge is still settling";
	case REAC_WAKE_SIBLING_SERVED:  return "other segments are served over this device";
	case REAC_WAKE_SPENT:           return "the wake ladder is spent";
	}
	return "unknown";
}
