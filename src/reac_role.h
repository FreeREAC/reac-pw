// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_role — the MASTER/SLAVE role selection, factored out of main.c so the
 * parse + validation contract is unit-testable without PipeWire/sockets.
 *
 * REAC has no fixed master: any box can be the master and the rest slave to it
 * (REAC-PROTOCOL-AND-TESTS.md §2/§4). openmixer must fit either role:
 *   - master (default): WE drive the cdea/cfea establishment and WE GENERATE THE
 *     PACE (the SCHED_FIFO pacer's cadence); a stagebox slaves to us.
 *   - slave: an EXTERNAL master drives; we RESPOND and lock to its cadence (frame
 *     arrival is our slot clock) + return our input channels upstream.
 * Both share the encoder/decoder + the PipeWire nodes; only WHO drives the
 * handshake and WHO GENERATES THE PACE differs.
 *
 * The role says nothing about the CLOCK REFERENCE. Generating the pace does not
 * make us the clock master — the cadence we generate should itself be disciplined
 * to a reference (a word clock, a PHC, locked graph hardware), and there is exactly
 * one clock master on a network. See reac_clock.h; keeping role / pace / clock
 * reference distinct is deliberate. */
#ifndef REAC_ROLE_H
#define REAC_ROLE_H

#include <string.h>

enum reac_role {
	REAC_ROLE_MASTER = 0,   /* default — preserves the original behaviour */
	REAC_ROLE_SLAVE,
};

/* Parse a --role argument string. Returns 0 + sets *out on success, -1 on an
 * unknown value (caller reports the error). NULL/"" is treated as unknown. */
static inline int reac_role_parse(const char *s, enum reac_role *out)
{
	if (!s)
		return -1;
	if (!strcmp(s, "master")) { *out = REAC_ROLE_MASTER; return 0; }
	if (!strcmp(s, "slave"))  { *out = REAC_ROLE_SLAVE;  return 0; }
	return -1;
}

/* The role's human name (for logs/usage). */
static inline const char *reac_role_name(enum reac_role r)
{
	return r == REAC_ROLE_SLAVE ? "slave" : "master";
}

/* Validate a parsed role against the other CLI options. The slave role REQUIRES a
 * TX NIC (the upstream return + handshake socket); the master role can run RX-only
 * (a pure monitor) or with --tx for the downstream sink. Returns 0 if OK, -1 if
 * the combination is invalid (caller reports which). `have_tx` = was --tx given. */
static inline int reac_role_validate(enum reac_role r, int have_tx)
{
	if (r == REAC_ROLE_SLAVE && !have_tx)
		return -1;   /* slave needs a NIC to return its inputs + handshake on */
	return 0;
}

#endif /* REAC_ROLE_H */
