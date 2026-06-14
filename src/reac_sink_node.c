// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* INTERFACE-ONLY skeleton. See reac_sink_node.h: the libreac TX layer this
 * depends on does not exist yet, so this registers ports for graph visibility
 * but emits no wire traffic. */

#include "reac_sink_node.h"
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <stdlib.h>

struct reac_sink_node {
	struct pw_filter *filter;  /* may be NULL in the stub build */
	struct reac_ring *tx_ring;
	struct reac_sink_cfg cfg;
};

struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg)
{
	(void)loop;
	struct reac_sink_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->tx_ring = tx_ring;
	n->cfg = *cfg;
	pw_log_warn("reac:playback sink scaffolded but inert — libreac TX layer "
	            "(frame builder + (s*N+ch)*3 interleaver + data[31] checksum + "
	            "u16-LE counter stamper) + SCHED_FIFO slot pacer are unbuilt; "
	            "no 0x8819 frames will be emitted (see NATIVE-REAC-DESIGN.md S2/S6).");
	/* A fuller build would here: pw_filter_new("reac:playback", Audio/Sink),
	 * add cfg->channels INPUT ports, connect RT_PROCESS, and start the pacer
	 * thread. Deliberately omitted until the TX layer lands. */
	return n;
}

void reac_sink_node_destroy(struct reac_sink_node *n)
{
	if (!n)
		return;
	if (n->filter)
		pw_filter_destroy(n->filter);
	free(n);
}
