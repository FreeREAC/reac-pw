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

#include <stdint.h>
#include <stdio.h>

#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

struct pw_loop;
struct reac_source_node;

/* Stable creation parameters for the reac-capture node: everything EXCEPT the
 * per-box width + label, which the autodetected box supplies. Bundled so the
 * recognition path can (re)size the node with one call without re-plumbing the
 * ring/rx/loop each time (they are process-lifetime constants). */
struct reac_pacer;   /* reac_pacer.h — the master engine's cadence + clock discipline */

struct reac_source_node_cfg {
	struct pw_loop   *loop;
	struct reac_ring *ring;
	struct reac_rx   *rx;
	int               sample_rate;
	const char       *inst;         /* per-instance node suffix (may be NULL)   */
	/* WHICH SEGMENT THIS NODE BELONGS TO, when that is not the same string as the
	 * node's own suffix. NULL (every role but `tap`) means it IS `inst`, which is
	 * the one-node-per-segment case every other role has.
	 *
	 * A TAP SERVES SEVERAL NODES OFF ONE SEGMENT — the desk's downstream and one
	 * per box MAC — so their suffixes must differ (`<seg>` and `<seg>.<mac>`) while
	 * `reac.segment` must NOT: a console keys a stagebox off that property, and
	 * three nodes claiming three segments where there is one is the same class of
	 * defect as the capture node whose reac.segment read `null` on 2026-09-08. The
	 * NAME is an address; the SEGMENT is an identity. */
	const char       *segment;
	int               master_role;  /* stamp the create-time badge props (#154) */
	/* THE GRAPH-CLOCK SAMPLE'S OTHER DOOR. This node is the one the console links
	 * (a box's inputs are what an operator patches first), so it is often the only
	 * one the graph drives — a reac-playback nobody has patched into is SUSPENDED
	 * and its callback never runs. Both nodes publish the same sample through
	 * reac_pacer_clock_publish_graph; NULL (a slave, a pcap run) publishes nothing
	 * and costs one branch. Borrowed, and it outlives this node: the pacer belongs
	 * to the segment's sink, which is torn down after every source rebuild. */
	struct reac_pacer *pacer;
	const char       *clock_ref;    /* REACPW_CLOCK_REF, forwarded verbatim */
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
 *   LINK-STATE LIVE UPDATES ARE NOT WIRED HERE: this node is constructed
 *   before reac_sink_node/reac_pacer exist (see main.c) and has no reference
 *   to the pacer or a timer of its own — see reac_sink_node.c's
 *   sink_publish_link_props for the live-update pattern reac-capture needs a
 *   pacer handle + a (shared) main-loop timer to reuse. The RATE half of live
 *   update IS wired this way — see reac_source_node_publish_rate below — via
 *   the same borrowed-timer pattern: the sink's peer_src slot reaches this
 *   node from the sink's own 200 ms poll, so no timer of this node's own is
 *   needed either.
 * Returns the node or NULL. */
/* `pacer` / `clock_ref`: the segment's clock discipline and the operator's designation,
 * taken HERE rather than assigned after the call, because the RT callback reads them and
 * pw_stream_connect can start it before a caller's next line runs. Both may be NULL. */
/* `segment` overrides the `reac.segment` identity; NULL means it is `inst` (see the
 * cfg field of the same name for why a tap needs the two to differ). */
struct reac_source_node *reac_source_node_new(struct pw_loop *loop,
                                              struct reac_ring *ring,
                                              struct reac_rx *rx,
                                              int sample_rate,
                                              int channels,
                                              const char *inst,
                                              const char *segment,
                                              const char *label,
                                              int master_role,
                                              struct reac_pacer *pacer,
                                              const char *clock_ref);

void reac_source_node_destroy(struct reac_source_node *n);

/* Live-update the reac-capture node's badge props (task #154 / issue #208). This node
 * is built before the pacer exists and has no pacer handle or timer of its own, so its
 * create-time seed (probing/none/0x0) would otherwise never change even after the box
 * establishes. The reac-playback sink — which DOES own the pacer + a main-loop timer —
 * pushes the recognized state here from that same (shared, single-loop) timer, so the
 * capture badge stops lying and tracks the box in lock-step with the playback side.
 * Args are the already-formatted strings the sink computes (reac.link-state name, box
 * model token, "INxOUT" width); a NULL string arg leaves that key untouched.
 * `box_mac48` is the PACKED box address (reac_mac48_pack) and is always stamped,
 * because 0 means NO BOX rather than "leave it alone" — the same composer the sink
 * uses turns it into reac.box.mac or REAC_BOX_MAC_NONE. No-op on a NULL node / one
 * with no filter yet. Main-loop thread only (same loop as the caller). */
void reac_source_node_publish_link(struct reac_source_node *n,
                                   const char *link_state,
                                   const char *box_model,
                                   const char *box_width,
                                   uint64_t box_mac48);

/* THE JOINED BOX MASTER'S IDENTITY on this node (DESIGN.md 0.5.2). The publish_link door
 * above is the MASTER's: the sink computes those strings from its pacer's recognition and
 * pushes them here. A receive-only join has no sink and no pacer — nothing recognises
 * anything, because the peer never declares itself — so its identity is composed from the
 * two facts the wire did give: the width it broadcasts and the address it broadcasts from.
 *
 * ONE COMPOSER, not a second spelling: this hands reac_box_master_identity_publish the same
 * pw_properties adapter publish_link uses, so reac.link-state / reac.box-model /
 * reac.box-width / reac.box.mac are written by exactly one function in this daemon.
 * Main-loop thread only. */
/* `enrolled` IS THE PAIRING, NOT THE HEARING (0.5.6, operator ruling). It used to be the
 * RX's own evidence — frames arriving and decoding — on 0.5.2's reasoning that a
 * receive-only join IS the slave role performed. That reasoning went with the receive-only
 * join: since 0.5.6 the daemon actually ENROLS with a box on M, and the console keys a
 * stagebox off `reac.link-state` (openmixer's `stageboxConnected`: only `established` is a
 * locked, streaming box). The rig said it plainly on 2026-09-09 — "S-0808 is not enrolled
 * but omx sees it available", with the box's own lamp unlocked. A box we can merely HEAR is
 * not a box we have joined, so this is now the slave engine's own ESTABLISHED flag:
 * `probing` while listening, flooding and waiting for the grant echo; `established` only
 * once the grant is accepted and the unicast stream and heartbeat are running. */
void reac_source_node_publish_box_master(struct reac_source_node *n,
                                         unsigned width, uint64_t mac48, int enrolled);

/* Live-update the reac-capture node's presented Format rate — the RATE half of
 * #208/2026-08-26-clock-tabs-and-reac-pace-coupling.md §1b ("a rate is ONE
 * wire rate — capture AND playback follow it together"). Mirrors reac_sink_
 * node.c's sink_reconnect_rate exactly: a same-object pw_stream_disconnect +
 * pw_stream_connect at a fresh Format pod (reac_sink_format_build, shared with
 * the sink — see reac_sink_format.h), on the SAME main-loop thread that already
 * owns this node, adopting `hz` only on a successful reconnect and
 * re-asserting the previous rate on failure (reac_sink_format_rate_after_
 * attempt decides which, honestly, either way). No-op when `hz` already
 * matches what this node presents (reac_sink_format_needs_update), so a
 * caller can call this on every poll tick with no churn when nothing moved.
 *
 * CALLER AND RATE SOURCE: this node has no pacer handle of its own (see the
 * LINK-STATE note above), so it does not decide when to reconnect — it is
 * PUSHED the rate to present. Today the only pusher is reac_sink_node.c's
 * sink_publish_rate_props, reached only via the sink's peer_src slot (main.c
 * wires that only for c->role == REAC_ROLE_MASTER — see reac_sink_node_set_
 * peer_source), so this function is exercised only for a MASTER's reac-
 * capture, following the pacer's rate_hz atomic: for a master that IS the
 * wire rate, so pushing it here is the same fact reaching the peer node, not
 * a second decision.
 *
 * SLAVE TODO: a slave's reac-capture rate is `reac_rx`'s own recovered/forced
 * wire rate (set once at reac_rx_open), and nothing today re-detects or
 * pushes a change to it live — there is no reac_sink_node/pacer on a slave's
 * side to drive a shared timer from at all. Wiring a slave rate change would
 * need reac_rx itself to notice + publish a new recovered rate first; that is
 * a reac_rx change, out of scope here, and not currently exercised by any
 * operator-facing control (a slave follows a foreign master; it does not get
 * asserted a rate the way `reac.cfg.rate` asserts one on a master). */
void reac_source_node_publish_rate(struct reac_source_node *n, int hz);

/* --- the SLAVE role's segment door + answer --------------------------------
 * A slave has no reac-playback node (main.c builds reac_sink_node for the master
 * branch alone), so this capture node carries both halves for a recorder: the
 * Props write door that accepts an assertion, and the segment's whole published
 * answer — identity, role trio, and the reac.master.* aggregate a mixer's sink
 * publishes on its own node. In the MASTER role the sink owns both and none of
 * these is called.
 *
 * THE IDENTITY IS STAMPED AT CREATE, in the slave role only: reac.segment names
 * the segment on the ONE node that carries its door, which is what lets a console
 * key a row on it and get one row per segment in either role
 * (reac_segment_ident.h). A master's reac-capture carries the same audio and is
 * NOT that segment's doorway, so it deliberately carries no reac.segment — one
 * store, one writer, and no de-duplication rule for a reader to get wrong. */

struct reac_role_swap;
struct reac_segment_answer;

/* Wire the SEGMENT's role lifecycle record (reac_role_swap.h), owned by the
 * listener because a swap destroys whichever node it started on. NULL detaches,
 * and the door is then inert. */
void reac_source_node_set_role_swap(struct reac_source_node *n, struct reac_role_swap *swap);

/* Publish the SLAVE segment's whole answer in ONE property update: the role trio
 * (`reac.role`, `reac.cfg.role.state`, `reac.cfg.role.refused`) and the
 * reac.master.* / reac.rate aggregate `answer` carries. One update rather than
 * two because these facts change together and a reader must never catch a role
 * that has moved against an aggregate that has not — the same consistency
 * reac.discovery.seq buys the sink's own set. A NULL `answer` publishes the role
 * trio alone. Main-loop thread only; same MERGE semantics as publish_link. */
void reac_source_node_publish_segment(struct reac_source_node *n,
                                      const char *role,
                                      const char *state,
                                      const char *refused,
                                      const struct reac_segment_answer *answer);

/* Take (read+clear) the pending accepted reac.cfg.role for a clean re-open in the
 * other engine, or -1 if none. Main's poll timer calls this. */
int reac_source_node_take_reopen_role(struct reac_source_node *n);


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
/* IS THIS NODE ACTUALLY ON THE GRAPH? A stream that was created and connected is not
 * yet a node: PipeWire assigns the node id asynchronously, and a connect that fails
 * later leaves an object nobody can see. Returns 1 once the daemon has given this
 * stream a node id and it is not in error; 0 otherwise, with *why (never NULL) naming
 * the state. The caller is expected to allow a grace period — CONNECTING is a normal
 * transient — and then to REBUILD rather than to keep reporting success, because a
 * segment whose capture node is missing has no input patches at all. */
int reac_source_node_on_graph(const struct reac_source_node *n, const char **why);

/* MAIN LOOP. Print anything the RT callback queued (one line, one atomic handshake —
 * the RT side never touches stdio). Safe with a NULL node. */
void reac_source_node_drain_log(struct reac_source_node *n, FILE *out);

int reac_source_node_ensure(struct reac_source_node **slot,
                            const struct reac_source_node_cfg *cfg,
                            int channels, const char *label);

#endif /* REAC_SOURCE_NODE_H */
