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
 * ONE NODE FOR THE PROCESS, EXCEPT WHEN A GROUP LEAVES. It is built once — lazily, on the
 * first publish, so a daemon that comes up before PipeWire does simply gets it on a later
 * tick — and every state change, width, model and provenance is a PROPERTY UPDATE, because
 * a node id that churns on a state change is a discovery storm in every client and this
 * node exists to end a discovery gap, not to open a different one.
 *
 * A REMOVAL IS THE ONE THING A PIPEWIRE NODE CANNOT BE TOLD (#106, spec amendment
 * 2026-09-20 §b). The documented way — a NULL value in the dict — is applied to the
 * CLIENT's copy: `pw_properties_update`'s `do_replace` deletes the item outright
 * (pipewire/properties.c), `pw_filter_update_properties` then publishes the SURVIVING keys
 * as `info.props` (pipewire/filter.c), and the server MERGES that with
 * `pw_properties_update_ignore` (pipewire/impl-node.c) — a key that is merely absent from a
 * merge is never removed. The desk read the result: `reac.roster.n = 0` (a set, delivered)
 * beside four stale `reac.roster.<i>.*` groups (removals, silently dropped), one of them
 * `established 32/8` on a wire with no carrier. There is no property-removal method on the
 * node interface either; this is a door that does not exist, not a bug to route around.
 *
 * SO A GROUP THAT LEAVES TAKES THE NODE WITH IT. On a tick whose roster is SHORTER than
 * what the node carries, the caller destroys this node, builds it again and republishes the
 * whole roster onto the fresh one. A segment DROPPING is rare and is already a discovery
 * event for every client on the graph; the alternative is a console reading a box that is
 * not there. `reac_roster_node_publish` REFUSES a removal rather than pretending it landed
 * — see its own contract below. */
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

/* Apply one delta from reac_roster_delta. Every entry must be a SET: a `remove` entry is
 * REFUSED (with `E_ROSTER_REMOVE`) and NOTHING in the delta is published, because a
 * half-applied roster is worse than a stale one — the caller's road for a departed group is
 * a node rebuild (see above). Returns 0 when the delta was published, -1 when it was
 * refused. */
int reac_roster_node_publish(struct reac_roster_node *n,
                             const struct reac_roster_kv *kv, int n_kv);

void reac_roster_node_destroy(struct reac_roster_node *n);

#endif /* REAC_ROSTER_NODE_H */
