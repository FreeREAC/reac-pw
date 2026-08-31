// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * reac_link_carrier — is there a CABLE in this interface?
 *
 * The predicate issue #95 was missing. reac_rx's predicates answer "is the interface there" and
 * "is it still the one I bound to"; neither notices the commoner case where the interface is
 * present and the cable is out.
 *
 * THE ARM THAT MATTERS IS THE UNKNOWN. A probe that cannot read must never be reported as
 * "down" — that is how a broken probe comes to look like a quiet wire, a shape this corpus has
 * paid for more than once. Every negative case below asserts -1, never 0, and `lo` is the
 * positive control so an all-(-1) result cannot pass as agreement.
 */
#include "reac_link.h"

#include <stdio.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	/* POSITIVE CONTROL FIRST: loopback is always up and always carries, on every host this
	 * can run on. Without it, a function that returned -1 unconditionally would pass. */
	CHK(reac_link_carrier("lo") == 1);

	/* A name short enough to BE an interface (IFNAMSIZ is 16) that does not exist: this
	 * reaches the open and tests the unreadable case, where an over-long name would bounce
	 * off the path-length guard instead and test nothing. */
	CHK(reac_link_carrier("nosuchif0") == -1);

	/* The guard itself, on purpose. */
	CHK(reac_link_carrier("reacpw-test-does-not-exist-9182") == -1);

	CHK(reac_link_carrier(NULL) == -1);
	CHK(reac_link_carrier("") == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_link_carrier reads the CABLE — lo carries, and every case it cannot "
	       "read answers UNKNOWN (-1) rather than down\n");
	return 0;
}
