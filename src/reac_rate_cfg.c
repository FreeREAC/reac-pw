// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_rate_cfg.h"
#include "reac_facts_pw.h"   /* REAC_SAMPLE_RATE_*: the legal paces, declared once */

#include <spa/param/props.h>
#include <spa/pod/pod.h>
#include <spa/pod/iter.h>

#include <stdio.h>
#include <string.h>

const char *reac_rate_refuse_code(enum reac_rate_refuse r)
{
	switch (r) {
	case REAC_RATE_REFUSE_NOT_CLOSED:   return "not_closed";
	case REAC_RATE_REFUSE_NOT_DRIVABLE: return "not_drivable";
	case REAC_RATE_REFUSE_ROLE_SLAVE:   return "role_slave";
	case REAC_RATE_REFUSE_MALFORMED:    return "malformed";
	case REAC_RATE_REFUSE_NONE:
	default:
		return "none";
	}
}

int reac_rate_is_closed(int hz)
{
	return hz == REAC_SAMPLE_RATE_44K1 || hz == REAC_SAMPLE_RATE_48K ||
	       hz == REAC_SAMPLE_RATE_96K;
}

unsigned reac_rate_bit(int hz)
{
	switch (hz) {
	case REAC_SAMPLE_RATE_44K1: return REAC_RATE_BIT_44100;
	case REAC_SAMPLE_RATE_48K:  return REAC_RATE_BIT_48000;
	case REAC_SAMPLE_RATE_96K:  return REAC_RATE_BIT_96000;
	default:    return 0;
	}
}

int reac_rate_best_drivable(unsigned drivable_mask)
{
	/* Highest first: "the default must be the best one that we can drive." */
	if (drivable_mask & REAC_RATE_BIT_96000) return REAC_SAMPLE_RATE_96K;
	if (drivable_mask & REAC_RATE_BIT_48000) return REAC_SAMPLE_RATE_48K;
	if (drivable_mask & REAC_RATE_BIT_44100) return REAC_SAMPLE_RATE_44K1;
	return 0;
}

size_t reac_rate_drivable_csv(unsigned drivable_mask, char *buf, size_t buflen)
{
	static const int ordered[] = { REAC_SAMPLE_RATE_44K1, REAC_SAMPLE_RATE_48K,
	                               REAC_SAMPLE_RATE_96K };
	char tmp[32];
	size_t len = 0;
	tmp[0] = '\0';
	for (size_t i = 0; i < sizeof ordered / sizeof ordered[0]; i++) {
		if (!(drivable_mask & reac_rate_bit(ordered[i])))
			continue;
		char cell[16];
		int n = snprintf(cell, sizeof cell, "%s%d", len ? "," : "", ordered[i]);
		if (n < 0)
			continue;
		if (len + (size_t)n < sizeof tmp) {
			memcpy(tmp + len, cell, (size_t)n);
			len += (size_t)n;
			tmp[len] = '\0';
		}
	}
	if (buflen > 0) {
		size_t copy = len < buflen - 1 ? len : buflen - 1;
		memcpy(buf, tmp, copy);
		buf[copy] = '\0';
	}
	return len;
}

enum reac_rate_refuse reac_rate_cfg_decide(enum reac_role role, int requested_hz,
                                           unsigned drivable_mask)
{
	/* A slave follows a foreign master's pace physically; it has no rate
	 * SETTING at all, so this is checked before the value is even looked at
	 * — a slave refuses every assertion the same way, valid or not. */
	if (role == REAC_ROLE_SLAVE)
		return REAC_RATE_REFUSE_ROLE_SLAVE;
	if (!reac_rate_is_closed(requested_hz))
		return REAC_RATE_REFUSE_NOT_CLOSED;
	if (!(drivable_mask & reac_rate_bit(requested_hz)))
		return REAC_RATE_REFUSE_NOT_DRIVABLE;
	return REAC_RATE_REFUSE_NONE;
}

/* Coerce a scalar value pod to a plain int, accepting the two encodings a
 * controller might naturally send for a sample rate: Int, or Float (a slider
 * or a JSON-number bridge that always emits floats). Mirrors
 * reac_headamp_prop.c's value_pod_to_byte, widened from a byte to a rate. */
static int value_pod_to_hz(const struct spa_pod *v, int *out)
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

int reac_rate_prop_parse(const struct spa_pod *props, int *out_hz)
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
	 * same carrier reac_headamp_prop_parse walks. */
	SPA_POD_STRUCT_FOREACH(&prop->value, child) {
		if (!key) {
			const char *s;
			if (spa_pod_get_string(child, &s) == 0)
				key = s;
			continue;
		}

		const char *k = key;
		key = NULL;                      /* consume the pair regardless of outcome */

		if (strcmp(k, REAC_CFG_PROP_RATE) != 0)
			continue;                    /* some other params entry (head-amp, ...) */

		int hz;
		if (value_pod_to_hz(child, &hz) != 0)
			return -1;                   /* the key was there; the value was not usable */
		*out_hz = hz;
		return 1;
	}
	return 0;
}
