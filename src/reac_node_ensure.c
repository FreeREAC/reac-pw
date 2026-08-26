// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_node_ensure — see reac_node_ensure.h for what this decides and why.

#include "reac_node_ensure.h"

#include <string.h>

bool reac_node_ensure_needs_rebuild(bool have_node, int cur_channels, const char *cur_label,
                                    int want_channels, const char *want_label)
{
	if (!have_node)
		return true;
	if (cur_channels != want_channels)
		return true;
	const char *a = cur_label ? cur_label : "";
	const char *b = want_label ? want_label : "";
	return strcmp(a, b) != 0;
}
