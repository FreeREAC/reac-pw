// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ROSTER's prop grammar and its DELTA — the two things the graph node is made of.
 * (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
 * 2026-09-16 third, §B.)
 *
 * WHY THE DELTA IS THE SUBJECT AND NOT THE PROPS. The ruling is "updated on every change,
 * props update, no node churn" — a roster that republishes everything every tick is a
 * client-side storm with the same node id, which is the defect one door down from the one
 * this node exists to close. So the assertion that matters is that nothing moved produces
 * NOTHING, and that a segment leaving REMOVES its keys rather than blanking them: an empty
 * string and an absent key must not read alike (the same law the daemon's `present` flag
 * is built on).
 *
 * PURE: no PipeWire, no netlink, no clock. tests/roster-node-lists-every-segment.sh is the
 * other half — every rule here can be right while main.c never publishes any of it. */
#include "reac_roster.h"

#include <stdio.h>
#include <string.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static struct reac_roster_kv kv[REAC_ROSTER_KV_MAX];

/* The value published for `key` in this delta, or NULL when the delta does not carry it.
 * `*removed` says which kind of carry it was. */
static const char *delta_val(const struct reac_roster_kv *d, int n, const char *key, int *removed)
{
	for (int i = 0; i < n; i++)
		if (strcmp(d[i].key, key) == 0) {
			if (removed)
				*removed = d[i].remove;
			return d[i].val;
		}
	return NULL;
}

static int carries(const struct reac_roster_kv *d, int n, const char *key, const char *val)
{
	int removed = 0;
	const char *v = delta_val(d, n, key, &removed);
	return v && !removed && strcmp(v, val) == 0;
}

int main(void)
{
	struct reac_roster r;
	reac_roster_init(&r);

	/* ---- 1. ONE EMPTY SEGMENT AND ONE BOXED ONE — the live case, exactly --------
	 * They are added in the order the daemon's tables hold them; the roster lists them
	 * in BYTE order of the name, because an index is an ORDER and not an identity. */
	reac_roster_begin(&r);
	CHECK(reac_roster_add(&r, "enp131s0.12", REAC_ROSTER_ESTABLISHED, "s1608",
	                      "master", "autodetected", REAC_BOX_S1608_IN, REAC_BOX_S1608_OUT) == 0, "add of a boxed segment refused");
	CHECK(reac_roster_add(&r, "enp131s0.11", REAC_ROSTER_PROBING, "none",
	                      "master", "autodetected", 0, 0) == 0, "add of an empty segment refused");
	int n = reac_roster_delta(&r, kv, REAC_ROSTER_KV_MAX);
	CHECK(n > 0, "the first roster produced no props at all (%d)", n);
	CHECK(carries(kv, n, "reac.roster.n", "2"), "the count is not 2: '%s'",
	      delta_val(kv, n, "reac.roster.n", NULL));
	CHECK(carries(kv, n, "reac.roster.0.name", "enp131s0.11"),
	      "group 0 is not the byte-first segment: '%s'",
	      delta_val(kv, n, "reac.roster.0.name", NULL));
	CHECK(carries(kv, n, "reac.roster.0.state", "probing"), "an empty segment is not `probing`");
	CHECK(carries(kv, n, "reac.roster.0.model", "none"),
	      "an empty segment's model is not `none` — an absent box must SAY none");
	CHECK(carries(kv, n, "reac.roster.0.role", "master"), "the resolved role is not published");
	CHECK(carries(kv, n, "reac.roster.0.source", "autodetected"), "the source is not published");
	CHECK(carries(kv, n, "reac.roster.0.width", "0/0"),
	      "an empty segment's width is not `0/0`: '%s'",
	      delta_val(kv, n, "reac.roster.0.width", NULL));
	CHECK(carries(kv, n, "reac.roster.1.name", "enp131s0.12"), "group 1 is not the second segment");
	CHECK(carries(kv, n, "reac.roster.1.state", "established"), "a boxed segment is not `established`");
	CHECK(carries(kv, n, "reac.roster.1.model", "s1608"), "the box model is not published");
	CHECK(carries(kv, n, "reac.roster.1.width", REACPW_STR(REAC_BOX_S1608_IN) "/" REACPW_STR(REAC_BOX_S1608_OUT)), "the width is not `in/out`: '%s'",
	      delta_val(kv, n, "reac.roster.1.width", NULL));
	reac_roster_commit(&r);

	/* ---- 2. NOTHING MOVED, SO NOTHING IS PUBLISHED ------------------------------
	 * The whole reason this module exists. Rebuild the identical roster and require an
	 * EMPTY delta: a tick that republishes is a client storm with the same node id. */
	reac_roster_begin(&r);
	reac_roster_add(&r, "enp131s0.12", REAC_ROSTER_ESTABLISHED, "s1608", "master",
	                "autodetected", REAC_BOX_S1608_IN, REAC_BOX_S1608_OUT);
	reac_roster_add(&r, "enp131s0.11", REAC_ROSTER_PROBING, "none", "master",
	                "autodetected", 0, 0);
	CHECK(reac_roster_delta(&r, kv, REAC_ROSTER_KV_MAX) == 0,
	      "an unchanged roster published props anyway");
	reac_roster_commit(&r);

	/* ---- 3. ONE STATE CHANGE MOVES ONE GROUP, AND ONLY WHAT CHANGED IN IT -------- */
	reac_roster_begin(&r);
	reac_roster_add(&r, "enp131s0.12", REAC_ROSTER_ESTABLISHED, "s1608", "master",
	                "autodetected", REAC_BOX_S1608_IN, REAC_BOX_S1608_OUT);
	reac_roster_add(&r, "enp131s0.11", REAC_ROSTER_ESTABLISHED, "s0808", "master",
	                "autodetected", REAC_BOX_S0808_IN, REAC_BOX_S0808_OUT);
	n = reac_roster_delta(&r, kv, REAC_ROSTER_KV_MAX);
	CHECK(carries(kv, n, "reac.roster.0.state", "established"), "the state change was not published");
	CHECK(carries(kv, n, "reac.roster.0.model", "s0808"), "the model change was not published");
	CHECK(carries(kv, n, "reac.roster.0.width", REACPW_STR(REAC_BOX_S0808_IN) "/" REACPW_STR(REAC_BOX_S0808_OUT)), "the width change was not published");
	CHECK(delta_val(kv, n, "reac.roster.0.name", NULL) == NULL,
	      "a key that did not change was published anyway");
	CHECK(delta_val(kv, n, "reac.roster.n", NULL) == NULL,
	      "the count was republished for a change that did not move it");
	CHECK(delta_val(kv, n, "reac.roster.1.state", NULL) == NULL,
	      "the OTHER segment's group was republished");
	reac_roster_commit(&r);

	/* ---- 4. A SEGMENT LEAVES: ITS KEYS ARE REMOVED, NOT BLANKED ------------------ */
	reac_roster_begin(&r);
	reac_roster_add(&r, "enp131s0.11", REAC_ROSTER_ESTABLISHED, "s0808", "master",
	                "autodetected", REAC_BOX_S0808_IN, REAC_BOX_S0808_OUT);
	n = reac_roster_delta(&r, kv, REAC_ROSTER_KV_MAX);
	CHECK(carries(kv, n, "reac.roster.n", "1"), "the count did not fall to 1");
	int removed = 0;
	CHECK(delta_val(kv, n, "reac.roster.1.name", &removed) != NULL && removed,
	      "the departed group's name key was not REMOVED");
	removed = 0;
	CHECK(delta_val(kv, n, "reac.roster.1.width", &removed) != NULL && removed,
	      "the departed group's width key was not REMOVED");
	reac_roster_commit(&r);

	/* ---- 5. THE STATE VOCABULARY IS EXACTLY THE SPEC'S --------------------------- */
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_PROBING), "probing") == 0, "probing");
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_ESTABLISHED), "established") == 0, "established");
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_SLAVE), "slave") == 0, "slave");
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_TAP), "tap") == 0, "tap");
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_REFUSED), "refused") == 0, "refused");
	CHECK(strcmp(reac_roster_state_name(REAC_ROSTER_IGNORED), "ignored") == 0, "ignored");

	/* ---- 6. THE BOUNDS ARE REPORTED, NEVER TRUNCATED IN SILENCE ------------------ */
	reac_roster_begin(&r);
	int refused = 0;
	for (int i = 0; i < REAC_ROSTER_MAX + 2; i++) {
		char nm[32];
		snprintf(nm, sizeof nm, "veth%02d", i);
		if (reac_roster_add(&r, nm, REAC_ROSTER_PROBING, "none", "auto", "autodetected", 0, 0) != 0)
			refused++;
	}
	CHECK(refused == 2, "%d segment(s) past the bound were refused, expected 2", refused);
	/* A delta that does not FIT refuses outright rather than delivering less than it was
	 * asked for — a truncated roster is a console reading a rig that is not there. */
	CHECK(reac_roster_delta(&r, kv, 4) == -1, "a delta that could not fit was truncated in silence");

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
