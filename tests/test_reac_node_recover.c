// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_node_recover — the bounded ladder behind "this node is not on the graph".
 *
 * The defect it answers (rig, 2026-09-08): a segment held its playback node and a journal
 * line reading "autodetected S-1608 -> reac-capture 16 in", with no capture node in the
 * graph, for NINE MINUTES. Every input patch on that box was dead and nothing reported it.
 *
 * The two ways of getting the answer wrong are equally real, so both are pinned here:
 * doing nothing (the defect), and retrying forever with no backoff — which floods the
 * journal, rebuilds against a graph that is already unhappy, and buries the one line an
 * operator needed. The decision is pure so it can be tested without arranging the
 * failure; that a rebuild REALLY rebuilds is main.c's job (it destroys the node first,
 * because reac_source_node_ensure only rebuilds on a width or label CHANGE and neither
 * moves when the node simply failed to appear).
 */
#include "reac_node_recover.h"

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* Run `n` ticks with the node absent, returning the first non-WAIT answer (and how many
 * ticks it took, through *at). */
static enum reac_node_recover_act run_absent(struct reac_node_recover *r, int n, int *at)
{
	for (int i = 1; i <= n; i++) {
		enum reac_node_recover_act a = reac_node_recover_step(r, 0);
		if (a != REAC_RECOVER_WAIT) {
			if (at)
				*at = i;
			return a;
		}
	}
	if (at)
		*at = n;
	return REAC_RECOVER_WAIT;
}

int main(void)
{
	struct reac_node_recover r;
	int at = 0;

	/* ---- A HEALTHY NODE IS NEVER TOUCHED. A thousand polls, no rebuild, no line. */
	reac_node_recover_init(&r);
	for (int i = 0; i < 1000; i++)
		CHK(reac_node_recover_step(&r, 1) == REAC_RECOVER_WAIT);
	CHK(r.attempts == 0);

	/* ---- THE GRACE IS REAL. A stream is legitimately not a node for a moment after it
	 * connects — PipeWire assigns the id asynchronously — so nothing happens inside the
	 * first window, and the rebuild lands exactly at its end. */
	reac_node_recover_init(&r);
	for (int i = 1; i < REAC_RECOVER_GRACE_TICKS; i++)
		CHK(reac_node_recover_step(&r, 0) == REAC_RECOVER_WAIT);
	CHK(reac_node_recover_step(&r, 0) == REAC_RECOVER_REBUILD);
	CHK(r.attempts == 1);

	/* ---- AND IT RESETS THE MOMENT THE NODE APPEARS. A node that came back mid-ladder
	 * is not "one attempt in"; the next absence gets the full budget. */
	CHK(reac_node_recover_step(&r, 1) == REAC_RECOVER_WAIT);
	CHK(r.attempts == 0 && r.absent == 0);
	run_absent(&r, REAC_RECOVER_GRACE_TICKS - 1, &at);
	CHK(reac_node_recover_step(&r, 0) == REAC_RECOVER_REBUILD);

	/* ---- THE WINDOW DOUBLES. A graph that refused us once usually refuses us again at
	 * once; a graph that is merely busy gets more time on each pass. Each attempt waits
	 * twice as long as the last, up to the ceiling. */
	reac_node_recover_init(&r);
	int expect = REAC_RECOVER_GRACE_TICKS;
	for (int attempt = 1; attempt <= REAC_RECOVER_MAX_ATTEMPTS; attempt++) {
		CHK(reac_node_recover_window(&r) == expect);
		CHK(run_absent(&r, expect, &at) == REAC_RECOVER_REBUILD);
		CHK(at == expect);            /* not one tick earlier, not one later */
		CHK(r.attempts == attempt);
		/* The message quotes the window that was SPENT, not the one now armed: the
		 * rig's first rebuild line read "4.0 s" for a node that had been missing 2. */
		CHK(reac_node_recover_spent(&r) == expect);
		if (attempt < REAC_RECOVER_MAX_SHIFT + 1)
			expect *= 2;
		else
			CHK(reac_node_recover_window(&r) ==
			    (REAC_RECOVER_GRACE_TICKS << REAC_RECOVER_MAX_SHIFT));
	}

	/* ---- IT IS BOUNDED, AND IT ENDS WITH ONE LINE. After the last attempt there is a
	 * single GIVE_UP and then silence — a repeated terminal message is noise, and noise
	 * is what hid the original defect. */
	CHK(run_absent(&r, 10000, &at) == REAC_RECOVER_GIVE_UP);
	CHK(r.gave_up == 1);
	for (int i = 0; i < 10000; i++)
		CHK(reac_node_recover_step(&r, 0) == REAC_RECOVER_WAIT);

	/* ---- GIVING UP IS ABOUT THIS ABSENCE, NOT ABOUT THE SEGMENT. The box is replugged,
	 * the session manager restarts, the node appears: the ladder starts over. */
	CHK(reac_node_recover_step(&r, 1) == REAC_RECOVER_WAIT);
	CHK(r.gave_up == 0 && r.attempts == 0);
	CHK(run_absent(&r, REAC_RECOVER_GRACE_TICKS, &at) == REAC_RECOVER_REBUILD);
	CHK(at == REAC_RECOVER_GRACE_TICKS);

	/* ---- A NULL is not a crash and not a rebuild. */
	CHK(reac_node_recover_step(NULL, 0) == REAC_RECOVER_WAIT);

	printf("ok: a missing node is rebuilt after a grace, with a doubling window, at most "
	       "%d times, and reported exactly once\n", REAC_RECOVER_MAX_ATTEMPTS);
	return 0;
}
