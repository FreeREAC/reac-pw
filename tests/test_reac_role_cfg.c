// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_role_cfg — the `reac.cfg.role` DECISION core
 * (2026-08-26-reac-runtime-config.md, the ROLE half). Pure, no socket, no RT
 * privilege — same style as test_reac_rate_cfg.c's part 1 (its own decision
 * core). Unlike rate there is no pacer-level section here, because a role change
 * does not happen inside one engine: master and slave are two of them, and the
 * swap plus the answer it owes afterwards belong to reac_role_swap
 * (tests/test_reac_role_swap.c). What this file covers, completely: the parse,
 * the same-role-is-a-no-op detection, and the state a request publishes with
 * nothing carrying it out — including that a role-changing assertion answers
 * `role_reestablish_pending` and never `applied`. */
#include "reac_role_cfg.h"
#include <reac/reac_role.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* ---- the write-side wire shape is ALREADY SHARED VOCABULARY ------------- *
 * libreac's include/reac/reac_cfg.h declares REAC_CFG_ROLE_PROP/_MASTER/
 * _SLAVE; this pins reac-pw's own copy to the exact values without needing
 * that sibling repo checked out (a build-time, not a source-parse, pin —
 * the cross-repo byte-for-byte parse lives in openmixer's conformance test
 * once a shared vocabulary increment mirrors this module's answer side). */
static int test_wire_shape_matches_libreac(void)
{
	CHK(strcmp(REAC_CFG_PROP_ROLE, "reac.cfg.role") == 0);
	CHK(REAC_CFG_ROLE_VALUE_MASTER == 0);
	CHK(REAC_CFG_ROLE_VALUE_SLAVE == 1);
	return 0;
}

static int test_refuse_code(void)
{
	CHK(strcmp(reac_role_refuse_code(REAC_ROLE_REFUSE_NONE), "none") == 0);
	CHK(strcmp(reac_role_refuse_code(REAC_ROLE_REFUSE_MALFORMED), "malformed") == 0);
	return 0;
}

static int test_changes_and_apply_state(void)
{
	/* Same role, either direction: a true no-op, answered "applied" — not a
	 * promise, already the fact. */
	CHK(reac_role_cfg_changes(REAC_ROLE_MASTER, REAC_ROLE_MASTER) == 0);
	CHK(reac_role_cfg_changes(REAC_ROLE_SLAVE, REAC_ROLE_SLAVE) == 0);
	CHK(strcmp(reac_role_cfg_apply_state(REAC_ROLE_MASTER, REAC_ROLE_MASTER),
	          REAC_ROLE_STATE_APPLIED) == 0);
	CHK(strcmp(reac_role_cfg_apply_state(REAC_ROLE_SLAVE, REAC_ROLE_SLAVE),
	          REAC_ROLE_STATE_APPLIED) == 0);

	/* A role CHANGE, either direction: accepted as well-formed, but the
	 * cross-engine swap never runs — the honest answer, never "applied". */
	CHK(reac_role_cfg_changes(REAC_ROLE_MASTER, REAC_ROLE_SLAVE) != 0);
	CHK(reac_role_cfg_changes(REAC_ROLE_SLAVE, REAC_ROLE_MASTER) != 0);
	CHK(strcmp(reac_role_cfg_apply_state(REAC_ROLE_MASTER, REAC_ROLE_SLAVE),
	          REAC_ROLE_STATE_REESTABLISH_PENDING) == 0);
	CHK(strcmp(reac_role_cfg_apply_state(REAC_ROLE_SLAVE, REAC_ROLE_MASTER),
	          REAC_ROLE_STATE_REESTABLISH_PENDING) == 0);

	/* Idempotent: asserting the SAME role twice in a row answers "applied"
	 * both times, not "applied" then something else the second time. */
	CHK(strcmp(reac_role_cfg_apply_state(REAC_ROLE_MASTER, REAC_ROLE_MASTER),
	          reac_role_cfg_apply_state(REAC_ROLE_MASTER, REAC_ROLE_MASTER)) == 0);

	/* SABOTAGE, verified by hand rather than left to trust: swapping which
	 * branch of reac_role_cfg_apply_state returns REESTABLISH_PENDING (i.e.
	 * inverting the reac_role_cfg_changes() ? : test) makes the two checks
	 * directly above fail — confirmed by temporarily inverting the ternary
	 * in reac_role_cfg.c and re-running this binary: both asserts above flip
	 * from PASS to FAIL, and reverting restores green. This guard is real. */
	return 0;
}

/* Open a Props object with a SPA_PROP_params struct, same helper shape as
 * test_reac_rate_cfg.c's begin_props/end_props. */
static void begin_props(struct spa_pod_builder *b, struct spa_pod_frame *obj,
                        struct spa_pod_frame *st)
{
	spa_pod_builder_push_object(b, obj, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, st);
}

static const struct spa_pod *end_props(struct spa_pod_builder *b,
                                       struct spa_pod_frame *obj,
                                       struct spa_pod_frame *st)
{
	spa_pod_builder_pop(b, st);
	return (const struct spa_pod *)spa_pod_builder_pop(b, obj);
}

static int test_prop_parse(void)
{
	uint8_t buf[1024];

	/* Well-formed master (0), interleaved with an unrelated rate key — the
	 * same params bag carries both (sink_build_params's single PropInfo). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, "reac.cfg.rate");
		spa_pod_builder_int(&b, 48000);
		spa_pod_builder_string(&b, REAC_CFG_PROP_ROLE);
		spa_pod_builder_int(&b, REAC_CFG_ROLE_VALUE_MASTER);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_SLAVE;   /* seed with the OTHER value */
		CHK(reac_role_prop_parse(pod, &r) == 1);
		CHK(r == REAC_ROLE_MASTER);
	}

	/* Well-formed slave (1). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_ROLE);
		spa_pod_builder_int(&b, REAC_CFG_ROLE_VALUE_SLAVE);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(pod, &r) == 1);
		CHK(r == REAC_ROLE_SLAVE);
	}

	/* A Float encoding (a toggle/JSON-number bridge) rounds to the nearest
	 * flag, same tolerance reac_rate_prop_parse gives a controller. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_ROLE);
		spa_pod_builder_float(&b, 1.0f);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(pod, &r) == 1);
		CHK(r == REAC_ROLE_SLAVE);
	}

	/* No reac.cfg.role key at all (a plain rate/head-amp write) -> 0,
	 * untouched. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, "reac.headamp.1.pad");
		spa_pod_builder_bool(&b, true);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(pod, &r) == 0);
		CHK(r == REAC_ROLE_MASTER);   /* untouched: the caller's variable is not stomped */
	}

	/* The key present, but a value neither Int nor Float can read -> -1
	 * (REAC_ROLE_REFUSE_MALFORMED territory). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_ROLE);
		spa_pod_builder_string(&b, "master");   /* the CLI's --role spelling, NOT the wire's */
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(pod, &r) == -1);
	}

	/* A numeric value that IS readable but is neither 0 nor 1 -> -1: the
	 * flag is closed to exactly two members, same "refused, never clamped"
	 * law rate's closed list obeys. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_ROLE);
		spa_pod_builder_int(&b, 2);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(pod, &r) == -1);
	}

	/* NULL / not a Props object at all -> -1. */
	{
		enum reac_role r = REAC_ROLE_MASTER;
		CHK(reac_role_prop_parse(NULL, &r) == -1);
	}

	return 0;
}

int main(void)
{
	CHK(test_wire_shape_matches_libreac() == 0);
	CHK(test_refuse_code() == 0);
	CHK(test_changes_and_apply_state() == 0);
	CHK(test_prop_parse() == 0);

	printf("OK: reac.cfg.role — the wire shape matches libreac's already-declared "
	       "vocabulary, the parse (Int/Float, absent, malformed, out-of-range), "
	       "same-role idempotency, and a role-changing assertion answering "
	       "role_reestablish_pending rather than a fake applied\n");
	return 0;
}
