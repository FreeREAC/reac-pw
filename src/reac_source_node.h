// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_source_node — the 40-channel REAC source node (the monitor box).
 *
 * A pw_filter registered as Audio/Source with REAC_MAX_CHANNELS output ports,
 * format F32 planar, rate = the recovered REAC rate. Its realtime process()
 * callback pulls one quantum per channel from the shared ring (filled by the
 * RX feeder) into the port buffers — and nothing else. PipeWire's adapter on
 * each outgoing link does channel-map / format-convert / RESAMPLE. We declare
 * the node a FOLLOWER and publish our measured rate error into io_rate_match so
 * the graph's async resampler tracks the desk's true rate (Tier-A clock
 * bridge). See NATIVE-REAC-DESIGN.md Section 3.4.
 */
#ifndef REAC_SOURCE_NODE_H
#define REAC_SOURCE_NODE_H

#include "reac_ring.h"
#include "reac_rx.h"

struct pw_loop;
struct reac_source_node;

/* Create + connect the source node onto the given PipeWire loop. Reads from
 * `ring`; reads `rx->ppm_error_milli` each cycle to drive io_rate_match.
 * `sample_rate` is the recovered REAC rate.
 *
 * `channels` = how many output ports to expose = the box's real input width
 *   (16 = S-1608, 8 = S-0808). <=0 or >40 falls back to the full 40-slot fabric.
 *   The box's inputs occupy ring slots 0..channels-1 (RX places them there), so
 *   exposing `channels` ports shows the box, not the fabric.
 * `inst`  = optional instance id -> node name "reac-capture.<inst>" so several
 *   masters (one per REAC VLAN/segment) coexist without a node-name clash.
 *   NULL/"" keeps the bare "reac-capture" (single-box / back-compat).
 * `label` = optional operator box name for the node DESCRIPTION only
 *   (e.g. "Drums" -> "Drums — 16 ch (REAC box inputs)"). Port names stay the stable
 *   "capture_NN" so a saved openmixer patch keeps linking; the box identity is on
 *   the node (name + description). NULL/"" -> the role-default description.
 * Returns the node or NULL. */
struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *label);

void reac_source_node_destroy(struct reac_source_node *n);

#endif /* REAC_SOURCE_NODE_H */
