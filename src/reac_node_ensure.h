// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_node_ensure — the ONE decision behind every "ensure this box's node
 * matches this width+label" call, factored out of reac_sink_node.c and
 * reac_source_node.c so it is provable without a live pw_stream (same
 * reasoning as reac_sink_format.h: PURE, no socket, no RT privilege).
 *
 * WHY THIS EXISTS: docs/design/notes/2026-08-26-duplicate-reac-node.md
 * measured two reac-playback nodes per segment on a clean boot — a `probing`
 * placeholder (box `none`) beside the `established` box-sized node. Tracing
 * both reac_sink_node_ensure and reac_source_node_ensure end to end shows
 * ONE struct per node, ONE pw_stream_new_simple call site each, and every
 * rebuild already destroys the prior stream before building the replacement
 * — there is no second creation path and no leaked pointer (this file's own
 * test proves that arithmetic offline). What the trace DID find is that the
 * two ensure() functions had DRIFTED: the sink's "is this the same box, or a
 * real change?" test compared width AND label; the source's compared width
 * ONLY. A box swap that keeps the same channel count but changes the label
 * (a re-enrolled box, or a relabelled pin) left reac-capture silently
 * carrying the OLD box's identity in its properties — a stale-identity bug
 * of exactly the shape the note describes, just on the capture side and
 * without a second pw_stream. Unifying the two callers onto one tested
 * decision closes that drift and gives both a shared, sabotage-verified
 * regression test. The live "two nodes" question is separate — see the
 * note's own "needs live pw-dump node count" framing; this module fixes what
 * is provable offline (the identity-comparison the destroy-or-keep call
 * hinges on) and no more. */
#ifndef REAC_NODE_ENSURE_H
#define REAC_NODE_ENSURE_H

#include <stdbool.h>

/* PURE decision: given the node that is (or is not) already live —
 * `have_node` false means no pw_stream exists yet, so a build is always
 * owed — does (want_channels, want_label) describe the SAME box as
 * (cur_channels, cur_label)? Returns true when a destroy+rebuild is owed
 * (absent, a width change, or a label change with the same width); false
 * only when every one of `have_node`/width/label already agrees, i.e. the
 * caller has nothing to do. NULL labels compare as "" (never a live-value
 * distinction — a box with no label and a box labelled "" are the same
 * fact), matching both callers' own `label ? label : ""` seeding. */
bool reac_node_ensure_needs_rebuild(bool have_node, int cur_channels, const char *cur_label,
                                    int want_channels, const char *want_label);

#endif
