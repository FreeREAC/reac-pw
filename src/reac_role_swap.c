// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_role_swap.h"

void reac_role_swap_init(struct reac_role_swap *s, enum reac_role boot_role)
{
	if (!s)
		return;
	s->asserted = boot_role;
	s->running = boot_role;
	s->engine_up = 0;
}

int reac_role_swap_request(struct reac_role_swap *s, enum reac_role want)
{
	if (!s)
		return 0;
	s->asserted = want;
	/* The swap is owed against the engine that is actually up, not against the
	 * last thing asked for: two assertions in one drain window must not cancel
	 * each other into a no-op that leaves the wrong engine running. */
	return s->engine_up ? s->running != want : 1;
}

void reac_role_swap_opened(struct reac_role_swap *s, enum reac_role role)
{
	if (!s)
		return;
	s->running = role;
	s->engine_up = 1;
}

void reac_role_swap_closed(struct reac_role_swap *s)
{
	if (!s)
		return;
	s->engine_up = 0;
}

enum reac_role_engine reac_role_engine_of_master(int engine_up, enum reac_master_state fsm)
{
	if (!engine_up)
		return REAC_ROLE_ENGINE_DOWN;
	/* IDLE is the transient before the first emitted frame; everything past it
	 * is a master pacing its own segment, box or no box. */
	return fsm == REAC_M_IDLE ? REAC_ROLE_ENGINE_HUNTING : REAC_ROLE_ENGINE_PERFORMING;
}

enum reac_role_engine reac_role_engine_of_slave(int engine_up, int established)
{
	if (!engine_up)
		return REAC_ROLE_ENGINE_DOWN;
	/* Flooding, cold-connecting and the post-grant mute dwell are all the HUNT:
	 * a recorder is not recording until a desk has enrolled it. */
	return established ? REAC_ROLE_ENGINE_PERFORMING : REAC_ROLE_ENGINE_HUNTING;
}

const char *reac_role_swap_state(const struct reac_role_swap *s,
                                 enum reac_role_engine engine)
{
	if (!s || !s->engine_up || engine == REAC_ROLE_ENGINE_DOWN)
		return REAC_ROLE_STATE_REESTABLISH_PENDING;
	if (s->running != s->asserted)
		return REAC_ROLE_STATE_REESTABLISH_PENDING;
	if (engine == REAC_ROLE_ENGINE_HUNTING)
		return REAC_ROLE_STATE_HUNTING;
	return REAC_ROLE_STATE_APPLIED;
}

int reac_role_emits_headamp(enum reac_role role)
{
	return role == REAC_ROLE_MASTER;
}
