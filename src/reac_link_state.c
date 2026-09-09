// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_link_state.h"
#include "reac_mac.h"

#include <stdio.h>

enum reac_link_state reac_link_state_from_master(enum reac_master_state st,
                                                  int just_dropped)
{
	if (just_dropped)
		return REAC_LINK_DROPPED;
	switch (st) {
	case REAC_M_GRANTING:
		return REAC_LINK_GRANTING;
	case REAC_M_ESTABLISHED:
		return REAC_LINK_ESTABLISHED;
	case REAC_M_IDLE:
	case REAC_M_PROBING:
	default:
		return REAC_LINK_PROBING;
	}
}

const char *reac_link_state_name(enum reac_link_state s)
{
	switch (s) {
	case REAC_LINK_PROBING:     return "probing";
	case REAC_LINK_GRANTING:    return "granting";
	case REAC_LINK_ESTABLISHED: return "established";
	case REAC_LINK_DROPPED:     return "dropped";
	default:                    return "probing";
	}
}

/* 17 characters plus the terminator. Private: the composed string has exactly one
 * destination — the stamp below — and handing callers a buffer to fill invites the
 * second, divergent formatting this key exists to retire. */
#define BOX_MAC_STR_CAP 18

void reac_box_mac_publish(uint64_t mac48, reac_prop_set_fn set, void *ctx)
{
	if (!set)
		return;
	char out[BOX_MAC_STR_CAP];
	if (mac48 == 0) {
		snprintf(out, sizeof out, "%s", REAC_BOX_MAC_NONE);
	} else {
		uint8_t m[6];
		reac_mac48_unpack(mac48, m);
		snprintf(out, sizeof out, "%02x:%02x:%02x:%02x:%02x:%02x",
		         m[0], m[1], m[2], m[3], m[4], m[5]);
	}
	set(ctx, REAC_PROP_BOX_MAC, out);
}

const struct reac_box_model *reac_box_master_model(unsigned width)
{
	size_t n = 0;
	const struct reac_box_model *t = reac_box_model_table(&n);
	if (!t)
		return NULL;
	for (size_t i = 0; i < n; i++)
		if (t[i].in_ch > 0 && (unsigned)t[i].in_ch == width)
			return &t[i];
	return NULL;
}

void reac_box_master_identity_publish(unsigned width, uint64_t mac48, int locked,
                                      reac_prop_set_fn set, void *ctx)
{
	if (!set)
		return;
	set(ctx, REAC_PROP_LINK_STATE,
	    reac_link_state_name(locked ? REAC_LINK_ESTABLISHED : REAC_LINK_PROBING));
	const struct reac_box_model *bm = reac_box_master_model(width);
	if (bm) {
		char w[16];
		snprintf(w, sizeof w, "%dx%d", bm->in_ch, bm->out_ch);
		set(ctx, REAC_PROP_BOX_MODEL, bm->token);
		set(ctx, REAC_PROP_BOX_WIDTH, w);
	}
	reac_box_mac_publish(mac48, set, ctx);
}
