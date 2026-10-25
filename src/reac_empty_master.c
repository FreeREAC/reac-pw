// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_empty_master: see reac_empty_master.h for when a master is streaming at nobody.

#include "reac_empty_master.h"

void reac_empty_master_init(struct reac_empty_master *e, uint64_t now_ns,
                            uint64_t rx_box_frames)
{
	e->rx_last = rx_box_frames;
	e->moved_ns = now_ns;
	e->was_past = 0;
}

int reac_empty_master_step(struct reac_empty_master *e, uint64_t now_ns,
                           uint64_t rx_box_frames, int past_probing)
{
	const int moved = rx_box_frames != e->rx_last;
	e->rx_last = rx_box_frames;
	if (moved)
		e->moved_ns = now_ns;
	if (past_probing) {
		/* A BOX IS ENROLLED: whether it is still there is libreac's ride-through to say. */
		e->was_past = 1;
		e->moved_ns = now_ns;
		return 0;
	}
	if (e->was_past)
		return 1;   /* libreac gave the box up: the stream stops now */
	return now_ns - e->moved_ns >= REAC_EMPTY_MASTER_QUIET_NS;
}
