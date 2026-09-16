// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* --role selection contract (reac_role.h), the exact parse + validation main.c
 * uses. No PipeWire, no socket: this fixes the role-config behaviour so a
 * regression in the flag handling is caught off-hardware.
 *
 *   - default is master (preserves the original behaviour);
 *   - "master"/"slave" parse to their roles, anything else is rejected;
 *   - the slave role REQUIRES --tx (the upstream return + handshake NIC);
 *   - the master role is valid with or without --tx (RX-only monitor, or the
 *     downstream sink). */
#include <reac/reac_hunt.h>
#include <reac/reac_role.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	enum reac_role r;

	/* default value before any --role: master (the struct/var initializer in main). */
	r = REAC_ROLE_MASTER;
	CHK(r == REAC_ROLE_MASTER);
	CHK(strcmp(reac_role_name(REAC_ROLE_MASTER), "master") == 0);
	CHK(strcmp(reac_role_name(REAC_ROLE_SLAVE), "slave") == 0);

	/* parse master/slave */
	CHK(reac_role_parse("master", &r) == 0 && r == REAC_ROLE_MASTER);
	CHK(reac_role_parse("slave", &r) == 0 && r == REAC_ROLE_SLAVE);

	/* reject unknown / NULL / empty */
	CHK(reac_role_parse("desk", &r) == -1);
	CHK(reac_role_parse("", &r) == -1);
	CHK(reac_role_parse(NULL, &r) == -1);

	/* validation: slave needs --tx, master does not */
	CHK(reac_role_validate(REAC_ROLE_SLAVE, /*have_tx=*/1) == 0);
	CHK(reac_role_validate(REAC_ROLE_SLAVE, /*have_tx=*/0) == -1);
	CHK(reac_role_validate(REAC_ROLE_MASTER, /*have_tx=*/1) == 0);
	CHK(reac_role_validate(REAC_ROLE_MASTER, /*have_tx=*/0) == 0);

	/* ---- THE FOURTH INTENT (2026-09-17 spec §1, §7) ---------------------------- */
	{
		enum reac_role_intent in;
		CHK(reac_role_intent_parse("box", &in) == 0 && in == REAC_ROLE_INTENT_BOX);
		CHK(strcmp(reac_role_intent_name(REAC_ROLE_INTENT_BOX), "box") == 0);
		/* A BOX IS THE SLAVE END OF THE PAIRING — the mixer drives, we answer — so it
		 * needs a TX NIC for exactly the reason a slave does. */
		CHK(reac_role_from_intent(REAC_ROLE_INTENT_BOX) == REAC_ROLE_SLAVE);
		CHK(reac_role_validate(reac_role_from_intent(REAC_ROLE_INTENT_BOX), 0) == -1);
		CHK(reac_role_validate(reac_role_from_intent(REAC_ROLE_INTENT_BOX), 1) == 0);
		/* It TRANSMITS — unlike the other explicit-only intent, which is the reason
		 * the two need a question of their own rather than being told apart by it. */
		CHK(reac_role_intent_transmits(REAC_ROLE_INTENT_BOX) == 1);
		CHK(reac_role_intent_transmits(REAC_ROLE_INTENT_TAP) == 0);
		/* EXPLICIT ONLY: an election may arrive at master or slave and at neither of
		 * these two. */
		CHK(reac_role_intent_explicit_only(REAC_ROLE_INTENT_BOX) == 1);
		CHK(reac_role_intent_explicit_only(REAC_ROLE_INTENT_TAP) == 1);
		CHK(reac_role_intent_explicit_only(REAC_ROLE_INTENT_AUTO) == 0);
		CHK(reac_role_intent_explicit_only(REAC_ROLE_INTENT_MASTER) == 0);
		CHK(reac_role_intent_explicit_only(REAC_ROLE_INTENT_SLAVE) == 0);
		/* And the hunt cannot spell it: every verdict it can reach names something
		 * else, checked against the verdict's own printer so a widened enum shows up
		 * here rather than on a desk. */
		for (int v = 0; v <= REAC_HUNT_REFUSED; v++) {
			const char *nm = reac_hunt_verdict_name((enum reac_hunt_verdict)v);
			CHK(nm && strcmp(nm, "box") != 0);
		}
	}

	printf("OK: --role parse (master default; master|slave; reject unknown) + validate "
	       "(slave requires --tx, master optional)\n");
	return 0;
}
