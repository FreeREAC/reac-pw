// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_roster_node — see reac_roster_node.h for what this node is and what it is not.

#include "reac_roster_node.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The node's name and the property a client finds it by. Stated here so the daemon, the
 * test and the documentation cannot drift apart. */
#define REAC_ROSTER_NODE_NAME  "reac-pw"
#define REAC_ROSTER_MEDIA_CLASS "Reac/Roster"

struct reac_roster_node {
	struct pw_filter *filter;
};

/* A filter with no ports has nothing to do, and says so: no process callback, no param
 * callback. The events struct still has to exist for pw_filter_new_simple. */
static const struct pw_filter_events roster_filter_events = {
	PW_VERSION_FILTER_EVENTS,
};

struct reac_roster_node *reac_roster_node_new(struct pw_loop *loop)
{
	if (!loop)
		return NULL;
	struct reac_roster_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	struct pw_properties *props = pw_properties_new(
		PW_KEY_NODE_NAME, REAC_ROSTER_NODE_NAME,
		PW_KEY_NODE_DESCRIPTION, "REAC segments (reac-pw)",
		/* A CLASS NO SESSION MANAGER KNOWS. Audio/* and Stream/* are what
		 * WirePlumber's linking and routing rules match on; this matches none of
		 * them, so the node is seen, read and left alone. */
		PW_KEY_MEDIA_CLASS, REAC_ROSTER_MEDIA_CLASS,
		/* THE DECLARATION A CLIENT FINDS IT BY. Never the node name: a name is an
		 * address, and a console that greps for one is a console that breaks when a
		 * second daemon runs under a different name. */
		"reac.roster", "1",
		NULL);
	if (!props) {
		free(n);
		return NULL;
	}
	n->filter = pw_filter_new_simple(loop, "reac:roster", props,
	                                 &roster_filter_events, n);
	if (!n->filter) {
		free(n);
		return NULL;
	}
	/* NO PORTS ADDED, and INACTIVE: this node carries no data and must never be
	 * scheduled. The connect is what exports it, which is the whole job. */
	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_INACTIVE, NULL, 0) < 0) {
		pw_filter_destroy(n->filter);
		free(n);
		return NULL;
	}
	return n;
}

void reac_roster_node_publish(struct reac_roster_node *n,
                              const struct reac_roster_kv *kv, int n_kv)
{
	if (!n || !n->filter || !kv || n_kv <= 0)
		return;
	/* A REMOVAL IS A NULL VALUE IN THE DICT, and that is why this builds a spa_dict by
	 * hand instead of a pw_properties: pw_properties_set(p, k, NULL) removes the key
	 * from p, so a removal can never survive into p's own dict. pw_properties_update —
	 * which is what pw_filter_update_properties runs over what we pass — reads a NULL
	 * value as "remove this key", which is the only way a departed segment leaves no
	 * trace on the node. */
	struct spa_dict_item *items = calloc((size_t)n_kv, sizeof *items);
	if (!items)
		return;
	for (int i = 0; i < n_kv; i++) {
		items[i].key = kv[i].key;
		items[i].value = kv[i].remove ? NULL : kv[i].val;
	}
	struct spa_dict dict = SPA_DICT_INIT(items, (uint32_t)n_kv);
	pw_filter_update_properties(n->filter, NULL, &dict);
	free(items);
}

void reac_roster_node_destroy(struct reac_roster_node *n)
{
	if (!n)
		return;
	if (n->filter) {
		pw_filter_disconnect(n->filter);
		pw_filter_destroy(n->filter);
	}
	free(n);
}
