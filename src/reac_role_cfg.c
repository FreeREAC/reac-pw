// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_role_cfg.h"

#include <spa/param/props.h>
#include <spa/pod/pod.h>
#include <spa/pod/iter.h>

#include <string.h>

const char *reac_role_refuse_code(enum reac_role_refuse r)
{
	switch (r) {
	case REAC_ROLE_REFUSE_MALFORMED: return "malformed";
	case REAC_ROLE_REFUSE_NONE:
	default:
		return "none";
	}
}

int reac_role_cfg_changes(enum reac_role current_role, enum reac_role requested_role)
{
	return current_role != requested_role;
}

const char *reac_role_cfg_apply_state(enum reac_role current_role,
                                      enum reac_role requested_role)
{
	/* Same role asserted: a true no-op, so "applied" is not a promise, it is
	 * already the fact. A different role: the invasive cross-engine swap
	 * (this header's HONESTY note) never runs here, so the answer says so
	 * instead of claiming a swap that never happened. */
	return reac_role_cfg_changes(current_role, requested_role)
		? REAC_ROLE_STATE_REESTABLISH_PENDING
		: REAC_ROLE_STATE_APPLIED;
}

/* Coerce a scalar value pod to a plain int, accepting the two encodings a
 * controller might naturally send for a two-state flag: Int, or Float (a
 * toggle/JSON-number bridge that always emits floats). Mirrors
 * reac_rate_cfg.c's value_pod_to_hz, which mirrors reac_headamp_prop.c's
 * value_pod_to_byte in turn — the same tolerance at a third width. */
static int value_pod_to_role_flag(const struct spa_pod *v, int *out)
{
	int32_t i;
	float f;
	if (spa_pod_get_int(v, &i) == 0) {
		*out = (int)i;
		return 0;
	}
	if (spa_pod_get_float(v, &f) == 0) {
		*out = (int)(f + 0.5f);
		return 0;
	}
	return -1;
}

int reac_role_prop_parse(const struct spa_pod *props, enum reac_role *out_role)
{
	if (!props || !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return -1;

	const struct spa_pod_object *obj = (const struct spa_pod_object *)props;
	const struct spa_pod_prop *prop = spa_pod_object_find_prop(obj, NULL,
	                                                           SPA_PROP_params);
	if (!prop || !spa_pod_is_struct(&prop->value))
		return 0;                        /* no cfg control carried this time */

	const char *key = NULL;              /* pending key awaiting its value pod */
	struct spa_pod *child;

	/* SPA_PROP_params is a flat Struct of alternating (String key, Pod value),
	 * same carrier reac_rate_prop_parse / reac_headamp_prop_parse walk. */
	SPA_POD_STRUCT_FOREACH(&prop->value, child) {
		if (!key) {
			const char *s;
			if (spa_pod_get_string(child, &s) == 0)
				key = s;
			continue;
		}

		const char *k = key;
		key = NULL;                      /* consume the pair regardless of outcome */

		if (strcmp(k, REAC_CFG_PROP_ROLE) != 0)
			continue;                    /* some other params entry (head-amp, rate, ...) */

		int flag;
		if (value_pod_to_role_flag(child, &flag) != 0)
			return -1;                   /* the key was there; the value was not usable */

		switch (flag) {
		case REAC_CFG_ROLE_VALUE_MASTER: *out_role = REAC_ROLE_MASTER; return 1;
		case REAC_CFG_ROLE_VALUE_SLAVE:  *out_role = REAC_ROLE_SLAVE;  return 1;
		default: return -1;              /* neither 0 nor 1: malformed */
		}
	}
	return 0;
}
