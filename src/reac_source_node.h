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

/* Stable creation parameters for the reac-capture node: everything EXCEPT the
 * per-box width + label, which the autodetected box supplies. Bundled so the
 * recognition path can (re)size the node with one call without re-plumbing the
 * ring/rx/loop each time (they are process-lifetime constants). */
struct reac_source_node_cfg {
	struct pw_loop   *loop;
	struct reac_ring *ring;
	struct reac_rx   *rx;
	int               sample_rate;
	const char       *inst;         /* per-instance node suffix (may be NULL)   */
	int               master_role;  /* stamp the create-time badge props (#154) */
};

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
 * `master_role` = 1 when this process runs --role master (a reac_pacer + master
 *   FSM exists, or is about to). Stamps the CREATE-TIME reac.link-state /
 *   reac.box-model / reac.box-width properties (task #154's stagebox-badge
 *   need) at "probing"/"none"/"0x0" — a real master enters PROBING
 *   unconditionally on its first frame (#130), so that is truthful from t=0.
 *   0 (slave role) omits the three keys entirely: the slave-side link state
 *   is a DIFFERENT state machine (reac_fsm/reac_ctrl) this task does not
 *   cover, and stamping a value from the wrong FSM would mislead a consumer.
 *   LIVE UPDATES ARE NOT WIRED HERE (follow-up): this node is constructed
 *   before reac_sink_node/reac_pacer exist (see main.c) and has no reference
 *   to the pacer or a timer of its own — see reac_sink_node.c's
 *   sink_publish_link_props for the live-update pattern reac-capture would
 *   need a pacer handle + its own (or a shared) main-loop timer to reuse.
 * Returns the node or NULL. */
struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *label,
                                              int master_role);

void reac_source_node_destroy(struct reac_source_node *n);

/* Bring *slot to a reac-capture node of `channels` output ports labelled `label`.
 * ONE entry point the library owns, callable from startup AND the recognition
 * path — it decides create vs. rebuild internally:
 *   - *slot == NULL           -> create it at `channels`.
 *   - exists, same width      -> no-op (input width is model-unique, so the same
 *                                 width is the same box).
 *   - exists, width changed    -> destroy the stale node and recreate at `channels`
 *                                 (a live box swap, e.g. S-4000S 32ch -> S-1608 16ch).
 * `channels` follows reac_source_node_new's clamp (<=0 or >40 -> the 40-ch fabric).
 * The shared ring/rx are unchanged across a resize, so the RX feeder keeps filling
 * and the rebuilt node reads the same planes. Input width is model-unique, so a
 * same-width call is a no-op. Returns 0, or -1 (on a failed rebuild *slot is left
 * NULL). */
int reac_source_node_ensure(struct reac_source_node **slot,
                            const struct reac_source_node_cfg *cfg,
                            int channels, const char *label);

#endif /* REAC_SOURCE_NODE_H */
