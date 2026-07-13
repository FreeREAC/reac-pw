// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_link_state.h"

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
