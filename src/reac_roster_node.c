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
	/* OUR OWN CORE, AND THAT IS THE POINT. pw_filter_connect() connects the core with a
	 * COPY OF THE FILTER'S PROPERTIES when the filter has none, so everything meant for
	 * the NODE lands on the CLIENT object too — see the decoy this cost, below. Owning
	 * the context and the core is what lets the client say only what a client should. */
	struct pw_context *context;
	struct pw_core    *core;
	struct pw_filter  *filter;
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
	/* THE NODE'S IDENTITY MUST NOT LAND ON THE CLIENT, and the shortest path puts it
	 * there. pw_filter_new_simple copies the properties it is given into the CONTEXT, and
	 * pw_filter_connect() connects the core with a COPY OF THE FILTER'S properties — so
	 * either way the CLIENT object ends up wearing node.name=reac-pw,
	 * media.class=Reac/Roster and reac.roster=1, with no roster on it at all.
	 *
	 * THAT COST A LIVE DEFECT REPORT ON THE DAY THIS SHIPPED. The operator looked for
	 * reac.roster, found the CLIENT's id first, ran `pw-cli info <id>`, saw three
	 * properties and no roster, and filed it against a daemon that was publishing
	 * correctly on the node next door. `reac.roster` is the declaration a client FINDS
	 * this node by, so exactly one object may wear it — anything else is a decoy that
	 * answers a search with silence.
	 *
	 * So we own the context and the core, give THEM only what a client should say about
	 * itself, and build the filter on that core with the node's own properties. */
	struct pw_properties *client_props = pw_properties_new(
		PW_KEY_APP_NAME, "reac-pw",
		NULL);
	if (!client_props) {
		free(n);
		return NULL;
	}
	n->context = pw_context_new(loop, client_props, 0);
	if (!n->context) {
		free(n);
		return NULL;
	}
	n->core = pw_context_connect(n->context, pw_properties_new(PW_KEY_APP_NAME, "reac-pw",
	                                                           NULL), 0);
	if (!n->core) {
		pw_context_destroy(n->context);
		free(n);
		return NULL;
	}
	struct pw_properties *props = pw_properties_new(
		PW_KEY_NODE_NAME, REAC_ROSTER_NODE_NAME,
		PW_KEY_NODE_DESCRIPTION, "REAC segments (reac-pw)",
		/* A CLASS NO SESSION MANAGER KNOWS. Audio/* and Stream/* are what
		 * WirePlumber's linking and routing rules match on; this matches none of
		 * them, so the node is seen, read and left alone. */
		PW_KEY_MEDIA_CLASS, REAC_ROSTER_MEDIA_CLASS,
		/* THE DECLARATION, ON THE NODE AND NOWHERE ELSE. Never the node name: a name
		 * is an address, and a console that greps for one breaks when a second daemon
		 * runs under a different name. */
		"reac.roster", "1",
		NULL);
	if (!props) {
		pw_core_disconnect(n->core);
		pw_context_destroy(n->context);
		free(n);
		return NULL;
	}
	n->filter = pw_filter_new(n->core, "reac:roster", props);
	if (!n->filter) {
		pw_core_disconnect(n->core);
		pw_context_destroy(n->context);
		free(n);
		return NULL;
	}
	/* NO PORTS ADDED, and INACTIVE: this node carries no data and must never be
	 * scheduled. The connect is what exports it, which is the whole job. */
	if (pw_filter_connect(n->filter, PW_FILTER_FLAG_INACTIVE, NULL, 0) < 0) {
		pw_filter_destroy(n->filter);
		pw_core_disconnect(n->core);
		pw_context_destroy(n->context);
		free(n);
		return NULL;
	}
	return n;
}

/* THE NODE'S ID ON THE GRAPH, or SPA_ID_INVALID until the export completes. The daemon
 * waits for it before announcing itself: an id in a log line that points at a different
 * object is worse than no id at all, and the operator's next command is `pw-cli info`. */
uint32_t reac_roster_node_id(const struct reac_roster_node *n)
{
	if (!n || !n->filter)
		return SPA_ID_INVALID;
	return pw_filter_get_node_id(n->filter);
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
	if (n->core)
		pw_core_disconnect(n->core);
	if (n->context)
		pw_context_destroy(n->context);
	free(n);
}
