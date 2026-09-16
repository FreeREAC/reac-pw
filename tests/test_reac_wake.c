// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_wake — the ladder that ends a silence no frame can end, and every refusal that
 * keeps it from being a port flapping at nobody.
 *
 * THE DEFECT, on the operator's desk 2026-09-16: 73 minutes of PROBING across two
 * processes, ~1620 completed scene pushes, 8003 frames/s leaving the NIC, rx_box_frames=0
 * from an S-1608 that had been enrolled before the desk suspended. Section A replays that
 * window minute by minute and requires the daemon to ACT. Against the behaviour this
 * module replaces — a master that never touches its own link — A fails on the first hour.
 *
 * The other half is asserted as hard: a bounce breaks every segment on the device, so the
 * facts that forbid it (no carrier, a carrier we cannot read, a box already talking, a
 * push we never finished, a sibling segment being served, a ladder already spent) are each
 * pinned with the exact refusal the operator will read in the journal.
 */
#include "reac_wake.h"

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

/* The live case's own shape: probing, carrier up, nothing back, our push completing on the
 * 2.6945 s cycle of a 96 kHz segment. */
static struct reac_wake_obs silent_at(uint64_t t0, uint64_t now)
{
	struct reac_wake_obs o = { 0 };
	o.probing = 1;
	o.carrier = 1;
	o.rx_box_frames = 0;
	o.scene_pushes = (now - t0) / (2694500ULL);   /* one completed transfer per cycle */
	o.siblings_served = 0;
	return o;
}

int main(void)
{
	struct reac_wake w;
	const uint64_t t0 = 5000ULL * SEC;
	uint64_t t;

	/* ---- A. THE LIVE WINDOW. Nothing happens while the cheap rung is still being
	 * played, and then the daemon acts — once. Stepped on a 100 ms grid over the first
	 * two minutes, so an edge one step early or one step late is caught. */
	reac_wake_init(&w, t0);
	int bounces = 0, exhausted = 0;
	uint64_t first_bounce = 0;
	for (t = t0; t < t0 + 120 * SEC; t += 100 * MS) {
		struct reac_wake_obs o = silent_at(t0, t);
		enum reac_wake_act a = reac_wake_step(&w, t, &o);
		if (a == REAC_WAKE_ACT_BOUNCE) {
			if (!bounces) first_bounce = t;
			bounces++;
		}
		/* The ladder is spent inside this window, so the one-shot lands here. */
		if (a == REAC_WAKE_ACT_EXHAUSTED) exhausted++;
		/* Nothing is ever refused as "spent" before an edge has been made. */
		if (!bounces) CHK(w.refusal != REAC_WAKE_SPENT);
	}
	/* THE ASSERTION THE OLD BEHAVIOUR FAILS: 73 minutes of this produced nothing at all.
	 * Two minutes of it must produce the edges the ladder allows and no more. */
	CHK(bounces == (int)REAC_WAKE_MAX_BOUNCES);
	/* The first edge waits for the grace floor AND for the pushes to be proven, and does
	 * not wait appreciably longer: the operator's box is mute for every second of it. */
	CHK(first_bounce >= t0 + REAC_WAKE_GRACE_NS);
	CHK(first_bounce <= t0 + REAC_WAKE_GRACE_NS + 1 * SEC);

	/* ---- B. THE LADDER IS SPENT, AND THE DAEMON SAYS SO EXACTLY ONCE. After that this
	 * segment is never bounced again however long it stays silent. */
	for (t = t0 + 120 * SEC; t < t0 + 3600 * SEC; t += SEC) {
		struct reac_wake_obs o = silent_at(t0, t);
		enum reac_wake_act a = reac_wake_step(&w, t, &o);
		CHK(a != REAC_WAKE_ACT_BOUNCE);
		if (a == REAC_WAKE_ACT_EXHAUSTED) exhausted++;
		else CHK(w.refusal == REAC_WAKE_SPENT);
	}
	CHK(exhausted == 1);

	/* ---- C. A PUSH THAT NEVER COMPLETES IS NEVER A LICENCE. The clock is the floor
	 * under the push count, never the trigger: an interrupted transfer is measured to
	 * produce nothing at all, so a master that has not finished one has not yet played
	 * the rung it is about to skip. An hour of it changes nothing. */
	reac_wake_init(&w, t0);
	for (t = t0; t < t0 + 3600 * SEC; t += SEC) {
		struct reac_wake_obs o = silent_at(t0, t);
		o.scene_pushes = REAC_WAKE_MIN_PUSHES - 1;   /* forever one short */
		CHK(reac_wake_step(&w, t, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_PUSH_NOT_PROVEN);
	}

	/* ---- D. EVERY REFUSAL, EACH NAMED. One well-past-grace instant, one fact changed
	 * at a time against an observation that would otherwise bounce. */
	{
		const uint64_t late = t0 + 60 * SEC;
		struct reac_wake_obs base = silent_at(t0, late);
		struct reac_wake_obs o;

		reac_wake_init(&w, t0);
		o = base; o.probing = 0;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_NOT_PROBING);

		reac_wake_init(&w, t0);
		o = base; o.carrier = 0;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_NO_CARRIER);

		reac_wake_init(&w, t0);
		o = base; o.carrier = -1;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_CARRIER_UNKNOWN);

		reac_wake_init(&w, t0);
		o = base; o.rx_box_frames = 1;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_BOX_IS_TALKING);

		reac_wake_init(&w, t0);
		o = base; o.siblings_served = 1;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_SIBLING_SERVED);

		/* And the control for all five: unchanged, it bounces. A refusal test that
		 * cannot show the same call succeeding proves nothing about the refusal. */
		reac_wake_init(&w, t0);
		o = base;
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_BOUNCE);
		CHK(w.refusal == REAC_WAKE_OK);
	}

	/* ---- E. THE SETTLE WINDOW. A box owes us its flood, its cold-connect and our grant
	 * dwell before it has failed to answer; a second edge inside that is the daemon
	 * talking over the box's reply. */
	{
		const uint64_t late = t0 + 60 * SEC;
		reac_wake_init(&w, t0);
		struct reac_wake_obs o = silent_at(t0, late);
		CHK(reac_wake_step(&w, late, &o) == REAC_WAKE_ACT_BOUNCE);
		for (t = late + SEC; t < late + REAC_WAKE_SETTLE_NS; t += SEC) {
			o = silent_at(t0, t);
			CHK(reac_wake_step(&w, t, &o) == REAC_WAKE_ACT_NONE);
			CHK(w.refusal == REAC_WAKE_SETTLING);
		}
		o = silent_at(t0, late + REAC_WAKE_SETTLE_NS);
		CHK(reac_wake_step(&w, late + REAC_WAKE_SETTLE_NS, &o) == REAC_WAKE_ACT_BOUNCE);
	}

	/* ---- F. RE-OPENING RESTARTS THE GRACE AND NOT THE LADDER. A box that enrols and
	 * drops again gets the cheap rung again; it does not get the port flapped once per
	 * drop, which is how a bounded remedy becomes an unbounded one. */
	{
		reac_wake_init(&w, t0);
		struct reac_wake_obs o = silent_at(t0, t0 + 60 * SEC);
		CHK(reac_wake_step(&w, t0 + 60 * SEC, &o) == REAC_WAKE_ACT_BOUNCE);
		CHK(w.bounces == 1);

		const uint64_t t1 = t0 + 600 * SEC;      /* it enrolled, it dropped again */
		reac_wake_reopen(&w, t1);
		CHK(w.bounces == 1);                     /* the ladder is where we left it */
		o = silent_at(t1, t1 + SEC);
		CHK(reac_wake_step(&w, t1 + SEC, &o) == REAC_WAKE_ACT_NONE);
		CHK(w.refusal == REAC_WAKE_PUSH_NOT_PROVEN);   /* the cheap rung, again */

		o = silent_at(t1, t1 + 60 * SEC);
		CHK(reac_wake_step(&w, t1 + 60 * SEC, &o) == REAC_WAKE_ACT_BOUNCE);
		CHK(w.bounces == REAC_WAKE_MAX_BOUNCES);
	}

	/* ---- G. EVERY REFUSAL HAS WORDS. A code the journal cannot print is a code the
	 * operator never reads. */
	for (int r = REAC_WAKE_OK; r <= REAC_WAKE_SPENT; r++) {
		const char *s = reac_wake_refusal_text((enum reac_wake_refusal)r);
		CHK(s != NULL && s[0] != '\0');
	}

	printf("reac_wake: the live window bounces %u times and no more; %s\n",
	       REAC_WAKE_MAX_BOUNCES, "eight refusals, each with its control");
	return 0;
}
