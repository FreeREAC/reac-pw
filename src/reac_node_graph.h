// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_node_graph — IS THIS STREAM REALLY A NODE ON THE GRAPH?
 *
 * One reading for both directions of a segment. reac_source_node_on_graph and
 * reac_sink_node_on_graph took the same three readings line for line — including the
 * same 2026-09-23 fix applied twice (#109) — and two copies of one law is how the pair
 * drifted the first time (reac_node_ensure.h tells that story for the width+label
 * comparison). The law:
 *
 *   A stream that was created and connected is NOT YET a node: PipeWire assigns the
 *   node id asynchronously, and a connect that fails later leaves an object nobody can
 *   see. It is on the graph once the daemon has given it a node id and it is neither in
 *   ERROR nor UNCONNECTED.
 *
 *   THE SERVER WENT AWAY UNDER IT. libpipewire answers a lost connection by putting the
 *   stream back to UNCONNECTED — not ERROR, and it keeps the node id it was given — so
 *   an id test and an error test both PASSED a node that no longer existed. Desk,
 *   2026-09-23 13:44: pipewire.service was restarted two seconds after an S-1608
 *   enrolled, and for twelve minutes the box's LED said enrolled, the roster said
 *   established, and the graph had nothing for it, with no line anywhere. A stream
 *   that is not connected is on no graph, whatever id it remembers
 *   (tests/graph-survives-a-pipewire-restart.sh).
 *
 * The callers allow a grace period (CONNECTING is a normal transient) and then REBUILD
 * rather than keep reporting success (reac_node_recover.h). */
#ifndef REAC_NODE_GRAPH_H
#define REAC_NODE_GRAPH_H

struct pw_stream;

/* Returns 1 when `stream` is on the graph, 0 otherwise — and then `*why` (never
 * NULL) names the reason: a NULL stream ("no node was ever created"), the stream's
 * own error text, the lost server, or the missing id. `why` itself may be NULL. */
int reac_node_on_graph(struct pw_stream *stream, const char **why);

#endif /* REAC_NODE_GRAPH_H */
