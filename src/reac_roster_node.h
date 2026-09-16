/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_roster_node — the ONE node the daemon always has, and the only thing on the graph
 * that is not a segment's door.
 * (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
 * 2026-09-16 third, §B. The table it publishes is reac_roster.h.)
 *
 * WHY A NODE AT ALL, when the ruling one amendment up REMOVED a node. Because the two
 * rulings are about different things. A zero-port node per SEGMENT is a device the console
 * renders and the operator must learn to ignore — `none / 0 in`, a row for a thing that is
 * not there. ONE node for the DAEMON is the daemon saying which wires it is on, which is
 * the fact the console lost when the per-segment doors went and which the operator's
 * second ruling of the day requires: "not autodetecting is an error" is a sentence only a
 * client that can SEE a probing segment can say.
 *
 * NOT LINKABLE, BY CONSTRUCTION AND NOT BY CONVENTION. It has NO PORTS — a pw_filter with
 * no port added, which is the one PipeWire object that is a node and carries nothing — and
 * `media.class = Reac/Roster`, a class no session manager has a rule for, so nothing links
 * it, routes it, or offers it as a device. `reac.roster = 1` is how a client FINDS it: a
 * name is an address and can be renamed, a property is a declaration.
 *
 * ONE NODE FOR THE PROCESS. It is built once — lazily, on the first publish, so a daemon
 * that comes up before PipeWire does simply gets it on a later tick — and lives until
 * exit. Every change is a PROPERTY UPDATE. A node id that churns on a state change is a
 * discovery storm in every client, and this node exists to end a discovery gap, not to
 * open a different one. */
#ifndef REAC_ROSTER_NODE_H
#define REAC_ROSTER_NODE_H

#include <stdint.h>

#include "reac_roster.h"

struct pw_loop;
struct reac_roster_node;

/* Build the node on `loop`. Returns NULL if PipeWire refused it — which is not fatal to
 * anything: the caller retries on the next tick, and a daemon with no graph still carries
 * audio and still has its journal. */
struct reac_roster_node *reac_roster_node_new(struct pw_loop *loop);

/* The node's id on the graph, or SPA_ID_INVALID while the export is still in flight. The
 * announcement waits for it: an operator reads a log line and runs `pw-cli info <id>`, and
 * the one thing that must never happen is that id landing on another object — which is
 * exactly how this node was reported broken on the day it shipped (the CLIENT wore its
 * properties; see reac_roster_node.c). */
uint32_t reac_roster_node_id(const struct reac_roster_node *n);

/* Apply one delta from reac_roster_delta. `remove` entries REMOVE the key (a NULL value in
 * the dict PipeWire merges), which is what makes a departed segment leave no trace. */
void reac_roster_node_publish(struct reac_roster_node *n,
                              const struct reac_roster_kv *kv, int n_kv);

void reac_roster_node_destroy(struct reac_roster_node *n);

#endif /* REAC_ROSTER_NODE_H */
