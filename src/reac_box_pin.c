// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_box_pin — see reac_box_pin.h for what a pin may and may not decide.

#include "reac_box_pin.h"

#include <stddef.h>
#include <string.h>

int reac_box_pin_parse(const char *spec,
                       const struct reac_box_model **model_out,
                       const char **label_out)
{
	if (!spec || !*spec || !model_out || !label_out)
		return -1;

	/* The model token is everything before the FIRST ':'; a label may itself contain
	 * colons ("Stage: left"), so only the first one separates. */
	size_t toklen = strcspn(spec, ":");

	/* ":Label" (no model) needs no check of its own: toklen is 0, and the whole-token
	 * match below would need a zero-length token to accept it, which no model has. An
	 * explicit early return here looked like a guard but no test could tell it from its
	 * absence — the house rule is that such a guard is decoration, so it is not here. */
	size_t n;
	const struct reac_box_model *table = reac_box_model_table(&n);
	const struct reac_box_model *found = NULL;
	for (size_t i = 0; i < n; i++) {
		/* WHOLE-token match: comparing only `toklen` bytes would accept "s16" as
		 * "s1608", and appending would make "s1608x" match too. Both ends must
		 * agree on the length. */
		if (strlen(table[i].token) == toklen &&
		    strncmp(spec, table[i].token, toklen) == 0) {
			found = &table[i];
			break;
		}
	}
	if (!found)
		return -1;

	const char *label = (spec[toklen] == ':') ? spec + toklen + 1 : NULL;
	/* An empty label is not a label. Naming a node "" is worse than naming it S-1608. */
	if (!label || !*label)
		label = found->display;

	*model_out = found;
	*label_out = label;
	return 0;
}
