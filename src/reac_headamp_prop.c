// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_headamp_prop.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */

#include <reac/reac.h>     /* REAC_MAX_CHANNELS */
#include <spa/param/props.h>
#include <spa/pod/pod.h>
#include <spa/pod/iter.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Coerce a scalar value pod to a byte, accepting the three encodings a
 * controller might naturally send: Bool (phantom/pad toggles), Int (any of the
 * three), or Float (a slider that carries sens as a float). Returns 0 on
 * success. Out-of-byte-range values are rejected so a stray huge number never
 * wraps into a valid-looking setting; the caller range-checks per param too. */
static int value_pod_to_byte(const struct spa_pod *v, uint8_t *out)
{
	bool b;
	int32_t i;
	float f;
	if (spa_pod_get_bool(v, &b) == 0) {
		*out = b ? 1 : 0;
		return 0;
	}
	if (spa_pod_get_int(v, &i) == 0) {
		if (i < 0 || i > 255)
			return -1;
		*out = (uint8_t)i;
		return 0;
	}
	if (spa_pod_get_float(v, &f) == 0) {
		if (f < 0.0f || f > 255.0f)
			return -1;
		*out = (uint8_t)(f + 0.5f);
		return 0;
	}
	return -1;
}

/* Map the trailing param name of a head-amp key to enum reac_headamp_param.
 * Returns -1 for an unknown name. */
static int param_name_to_id(const char *name)
{
	if (strcmp(name, "phantom") == 0)
		return REAC_HEADAMP_PHANTOM;
	if (strcmp(name, "pad") == 0)
		return REAC_HEADAMP_PAD;
	if (strcmp(name, "sens") == 0)
		return REAC_HEADAMP_SENS;
	return -1;
}

/* Parse "reac.headamp.<ch>.<param>" into ch + param id. Returns 0 on success.
 * The prefix is already known to match; `rest` points just past it. */
static int parse_key(const char *rest, uint8_t *ch, uint8_t *param)
{
	/* <ch> is a decimal channel index terminated by '.'. */
	char *dot = NULL;
	long c = strtol(rest, &dot, 10);
	if (dot == rest || *dot != '.')
		return -1;                       /* no digits, or no '.' after them */
	if (c < 0 || c >= REAC_MAX_CHANNELS)
		return -1;
	int p = param_name_to_id(dot + 1);
	if (p < 0)
		return -1;
	*ch = (uint8_t)c;
	*param = (uint8_t)p;
	return 0;
}

/* Per-param absolute-value range gate (mirrors reac_headamp_tx_set's own check,
 * applied here so the parse drops bad values rather than handing them on). */
static int value_in_range(uint8_t param, uint8_t value)
{
	switch (param) {
	case REAC_HEADAMP_PHANTOM:
	case REAC_HEADAMP_PAD:
		return value <= 1;
	case REAC_HEADAMP_SENS:
		return value <= REAC_HEADAMP_SENS_MAX;
	default:
		return 0;
	}
}

int reac_headamp_prop_parse(const struct spa_pod *props,
                            struct reac_headamp_setting *out, int max)
{
	if (!props || !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return -1;

	const struct spa_pod_object *obj = (const struct spa_pod_object *)props;
	const struct spa_pod_prop *prop = spa_pod_object_find_prop(obj, NULL,
	                                                           SPA_PROP_params);
	if (!prop || !spa_pod_is_struct(&prop->value))
		return 0;                        /* no head-amp control carried this time */

	const size_t prefix_len = strlen(REAC_HEADAMP_PROP_PREFIX);
	int n = 0;
	const char *key = NULL;              /* pending key awaiting its value pod */
	struct spa_pod *child;

	/* SPA_PROP_params is a flat Struct of alternating (String key, Pod value). */
	SPA_POD_STRUCT_FOREACH(&prop->value, child) {
		if (!key) {
			const char *s;
			/* A malformed non-string in a key slot desyncs the pairing; skip it
			 * and keep the key slot open so we re-sync on the next String. */
			if (spa_pod_get_string(child, &s) == 0)
				key = s;
			continue;
		}

		const char *k = key;
		key = NULL;                      /* consume the pair regardless of outcome */

		if (strncmp(k, REAC_HEADAMP_PROP_PREFIX, prefix_len) != 0)
			continue;                    /* some other params entry */

		uint8_t ch, param, value;
		if (parse_key(k + prefix_len, &ch, &param) != 0)
			continue;
		if (value_pod_to_byte(child, &value) != 0)
			continue;
		if (!value_in_range(param, value))
			continue;

		if (n >= max)
			break;                       /* out is full: stop, never overrun */
		out[n].ch = ch;
		out[n].param = param;
		out[n].value = value;
		n++;
	}
	return n;
}
