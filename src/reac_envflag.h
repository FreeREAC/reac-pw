// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_envflag — ONE reading of a boolean knob, for every boolean knob.
 *
 * Three knobs had grown three spellings of the same question: REACPW_GRANT_ON_DECLARE
 * asked "is the first byte one of 0/n/N/f/F", REACPW_RATE_MATCH asked "does atoi()
 * differ from zero", and REACPW_CLOCK_FOLLOW asked only "is it SET" — so
 * `REACPW_CLOCK_FOLLOW=0` turned the discipline ON, which is the opposite of what
 * anybody typing it means. A knob that reads its own value differently from its
 * neighbour is a trap laid for the operator, not a feature.
 *
 * The contract, for all of them: an UNSET or EMPTY variable is the DEFAULT (an empty
 * value is a key someone blanked out — reac_conf.h's own rule, applied here), and a set
 * value is read for truth: 0/n/no/f/false/off are false, 1/y/yes/t/true/on are true.
 * Anything else is REPORTED by the caller and falls back to the default rather than
 * being silently taken as true — a value nobody can parse is not consent.
 *
 * These are PROCESS-WIDE knobs (layer 2 of reac_conf.h's precedence), read once at
 * startup. A per-segment fact does not belong here; it belongs in a suffixed key
 * resolved through reac_conf_lookup. */
#ifndef REAC_ENVFLAG_H
#define REAC_ENVFLAG_H

#include <stdlib.h>
#include <string.h>

/* Parse one boolean word. Returns 1/0 on a recognized value, -1 when the string is
 * NULL, empty or not a boolean at all (the caller decides: default, or report). */
static inline int reac_envflag_parse(const char *v)
{
	if (!v || !*v)
		return -1;
	if (!strcasecmp(v, "0") || !strcasecmp(v, "n") || !strcasecmp(v, "no") ||
	    !strcasecmp(v, "f") || !strcasecmp(v, "false") || !strcasecmp(v, "off"))
		return 0;
	if (!strcasecmp(v, "1") || !strcasecmp(v, "y") || !strcasecmp(v, "yes") ||
	    !strcasecmp(v, "t") || !strcasecmp(v, "true") || !strcasecmp(v, "on"))
		return 1;
	return -1;
}

/* The value of environment variable `name`, or `dflt` when it is unset, empty or
 * unparseable. */
static inline int reac_envflag(const char *name, int dflt)
{
	int v = reac_envflag_parse(getenv(name));
	return v < 0 ? (dflt ? 1 : 0) : v;
}

#endif /* REAC_ENVFLAG_H */
