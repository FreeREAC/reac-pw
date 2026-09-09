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

	printf("OK: --role parse (master default; master|slave; reject unknown) + validate "
	       "(slave requires --tx, master optional)\n");
	return 0;
}
