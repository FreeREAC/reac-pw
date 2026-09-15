// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The two pieces of reac_qdisc that can be pinned without a wire.
 *
 * 1. THE ETF CATCH-UP BUDGET IS A DURATION, and the bug it replaces was a units
 *    bug of exactly the shape reac_pacer.h already warned about: a budget written
 *    for one reference (the thread's wake = the egress instant) left in force under
 *    a backend that moved the reference (the kernel's launch time, a lead ahead).
 *    These cases pin the arithmetic AND the two edges — a lead inside the qdisc's
 *    own delta cannot absorb anything, and 0 is not an answer this may return,
 *    because 0 means "the library default" to the pacer's cfg.
 *
 * 2. THE STATS READ SAYS UNREADABLE WHEN IT IS. A dump that completed and found no
 *    etf qdisc on the device answers 0 with qdiscs == 0; a call that could not ask
 *    answers -errno. Reporting 0 drops from a read that never happened is the
 *    absence-looks-like-silence failure this whole lane is about.
 *
 * No socket beyond one netlink dump of the host's own qdisc table, which is a read
 * and changes nothing. Safe beside a live rig. */
#include "reac_qdisc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

int main(void)
{
	/* The shipped default: 2500 us lead, 300 us qdisc delta -> 2200 us repayable. */
	CHECK(reac_qdisc_etf_catchup_slots(2500, 8000) == 17,
	      "2500 us lead at 8000 fps is 17 slots, got %d",
	      reac_qdisc_etf_catchup_slots(2500, 8000));
	CHECK(reac_qdisc_etf_catchup_slots(2500, 4000) == 8,
	      "2500 us lead at 4000 fps is 8 slots, got %d",
	      reac_qdisc_etf_catchup_slots(2500, 4000));
	CHECK(reac_qdisc_etf_catchup_slots(2500, 3675) == 8,
	      "2500 us lead at 3675 fps is 8 slots, got %d",
	      reac_qdisc_etf_catchup_slots(2500, 3675));

	/* THE BUDGET IT REPLACES. libreac's rate-derived default is 1000 us — 8 slots at
	 * 8000 fps — and the whole finding is that it is SMALLER than what the lead can
	 * absorb, so a wake the lead covers still re-based the launch grid. If this ever
	 * stops being true the fix has stopped being a fix. */
	CHECK(reac_qdisc_etf_catchup_slots(2500, 8000) > 8,
	      "the ETF budget must exceed the thread backend's 1000 us default, "
	      "or nothing changed");

	/* A lead at or inside the qdisc's delta absorbs nothing — but 0 means "use the
	 * library default" to the cfg field this feeds, so the floor is one slot. */
	CHECK(reac_qdisc_etf_catchup_slots(300, 8000) == 1,
	      "a lead equal to the qdisc delta floors at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(300, 8000));
	CHECK(reac_qdisc_etf_catchup_slots(50, 8000) == 1,
	      "a lead inside the qdisc delta floors at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(50, 8000));
	CHECK(reac_qdisc_etf_catchup_slots(2500, 0) == 1,
	      "a zero rate cannot divide: floor at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(2500, 0));

	/* Refusals by argument, not by guess. */
	struct reac_qdisc_stats st;
	CHECK(reac_qdisc_stats_read(0, &st) == -EINVAL, "ifindex 0 is refused");
	CHECK(reac_qdisc_stats_read(1, NULL) == -EINVAL, "a NULL out is refused");

	/* A DUMP THAT COMPLETES AND FINDS NOTHING IS NOT AN ERROR, and it is not a
	 * silent zero either: `qdiscs` says the sum covers nothing. ifindex 0x7ffffffe
	 * exists on no machine, so this is the "device carries no etf" answer. */
	memset(&st, 0xAA, sizeof st);
	int rc = reac_qdisc_stats_read(0x7ffffffe, &st);
	if (rc == 0) {
		CHECK(st.qdiscs == 0, "a device with no etf qdisc reports qdiscs=0, got %u",
		      st.qdiscs);
		CHECK(st.drops == 0, "and no drops, got %llu", st.drops);
	} else {
		/* No rtnetlink here (a sandbox with no NETLINK_ROUTE). Say so — a skip
		 * that prints nothing is the absence that looks like a pass. */
		printf("note: no rtnetlink in this environment (errno %d); the dump arm "
		       "did not run\n", -rc);
	}

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
