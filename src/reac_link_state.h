// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_link_state — the PURE mapping from the MASTER FSM (reac_master_state)
 * onto the small badge vocabulary a PipeWire consumer (openmixer's stagebox
 * card) reads off the reac-capture / reac-playback node properties, so it can
 * show "no box / probing / connected" without inferring it from wire traffic.
 *
 * Deliberately free of PipeWire/atomics/I/O — a pure function the unit test
 * exercises directly and the node-advertisement shells (reac_sink_node.c,
 * reac_source_node.c) call from the non-RT main loop only.
 *
 * The badge vocabulary is COARSER than reac_master_state:
 *   - REAC_M_IDLE and REAC_M_PROBING both read as "probing" — a real master
 *     enters PROBING unconditionally on its very first emitted frame (#130),
 *     so IDLE is a sub-millisecond transient never worth a distinct badge.
 *   - REAC_M_GRANTING reads as "granting" (the box just cold-connected; the
 *     grant burst is in flight).
 *   - REAC_M_ESTABLISHED reads as "established" (linked).
 *   - "dropped" is NOT a persisted FSM state (a backward transition lands
 *     back in PROBING the same tick) — it is an EVENT overlay the caller
 *     requests via `just_dropped` for the exact non-RT drain cycle in which
 *     it observed a backward transition (ESTABLISHED/GRANTING -> PROBING),
 *     read off reac_pacer's drops[] counters. A PipeWire property-change
 *     listener (the intended consumer) sees that one update as a distinct
 *     event even though the NEXT poll settles back to "probing" — exactly
 *     the "briefly flash, then re-probe" signal the badge wants for
 *     "lost the box" vs. "never had one".
 */
#ifndef REAC_LINK_STATE_H
#define REAC_LINK_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "reac_master.h"

/* PipeWire node property keys the badge consumer reads. reac.link-state is
 * the must-have; reac.box-model / reac.box-width are best-effort (populated
 * once the master's box recognizer matches a fixed-matrix model; "none" /
 * "0x0" until then — see reac_ctrl.h's reac_box_model). */
#define REAC_PROP_LINK_STATE "reac.link-state"
#define REAC_PROP_BOX_MODEL  "reac.box-model"
#define REAC_PROP_BOX_WIDTH  "reac.box-width"

/* The box's OWN identity, decoded from the identity-page replies (DT1 tag 0x0500)
 * the grant sweep polls — beside reac.box-model, which comes from the geometry.
 * reac.box-firmware is the version as "D.DDD" (S-0808 1.003, …); reac.box-hw is
 * the raw hardware-identity block as hex, carried UNINTERPRETED (its meaning is
 * unresolved). Both are the EMPTY STRING until the box answers the poll (and again
 * once it drops) — a consumer reads "" as "not answered", a fact, not a zero. */
#define REAC_PROP_BOX_FIRMWARE "reac.box-firmware"
#define REAC_PROP_BOX_HW       "reac.box-hw"

/* THE ENROLLED BOX'S OWN L2 ADDRESS — the peer's, never ours. Latched from the
 * source MAC of the box's JOIN (the `cdea 04 03` the master answers with a grant)
 * and cleared with the box, so it says exactly which chassis is on this segment.
 * Colon-separated lowercase hex, the form the master's own log prints; the string
 * REAC_BOX_MAC_NONE while no box is known.
 *
 * IT IS PUBLISHED BECAUSE reac.master.mac IS NOT IT AND CANNOT BE. That key names
 * whoever drives the segment, which in the master role is THIS machine's NIC. A
 * consumer that keys a stagebox registry by box address — openmixer keys its
 * `reac:<box mac>` patch names exactly that way — read reac.master.mac, matched
 * our own NIC against a registry of Roland addresses, and missed on every rig
 * (openmixer docs/design/notes/2026-09-06-rig-headamp-and-clip-findings.md §5).
 * The alternative was for every consumer to re-derive the peer from a discovery
 * sighting list, which is a second implementation of a fact only the master
 * actually holds. */
#define REAC_PROP_BOX_MAC      "reac.box.mac"
#define REAC_BOX_MAC_NONE      "none"


/* WHERE the box identity came from. There is exactly one possible answer while a
 * box is known — the wire — and saying so explicitly is the point: reac-pw has no
 * configured box any more (the master's --box was retired 2026-08-05), so a
 * consumer can trust reac.box-model without asking whether an operator typed it.
 * A consumer that finds this key absent is talking to a reac-pw that COULD still
 * be running a pinned model and should treat the model as unattributed.
 *
 * REAC_PROP_HEADAMP_BASE is the head-amp WIRE CHANNEL the box's input 1 sits at —
 * `base` in `CH = base + (input - 1)`. It is published because the alternative is
 * every consumer re-deriving it from the width against its own copy of the
 * placement table (openmixer does exactly that today), which is a second
 * implementation of a policy only reac-pw can actually observe, and it is wrong
 * the moment the placement rule gains a case. Decimal; REAC_BOX_SOURCE_NONE when
 * no box is known, so a numeric parse fails rather than reading as base 0. */
#define REAC_PROP_BOX_SOURCE   "reac.box-source"
#define REAC_PROP_HEADAMP_BASE "reac.headamp.base"

/* reac.box-source values. */
#define REAC_BOX_SOURCE_WIRE "wire"   /* the box declared itself; we matched it */
#define REAC_BOX_SOURCE_NONE "none"   /* no box on this wire — a normal state   */

/* reac.discovery.* — passive discovery (task #178): what is ON THE SEGMENT, as opposed
 * to the reac.link-state/box-* keys above, which describe only the peer THIS master
 * joined. Same seam, same node, same 200 ms poll; see reac_disco.h for the rules that
 * keep the device list honest, and openmixer's
 * docs/design/specs/2026-07-16-reac-discovery-via-reac-pw.md for the contract.
 *
 * A reader that finds reac-playback WITHOUT these keys is talking to a reac-pw that
 * predates discovery — which is "could not scan", never "scanned and found nothing". */
#define REAC_PROP_DISCO_SCOPE   "reac.discovery.scope"    /* the one NIC observed */
#define REAC_PROP_DISCO_STATE   "reac.discovery.state"    /* REAC_DISCO_STATE_* */
#define REAC_PROP_DISCO_SEQ     "reac.discovery.seq"      /* change counter; frozen = wedged */
#define REAC_PROP_DISCO_DEVICES "reac.discovery.devices"  /* JSON array snapshot */

/* THE SEGMENT AGGREGATE (arbitration spec §1). Sightings say what was HEARD; these say what
 * the segment IS, which is the thing a consumer actually needs and which every reader was
 * otherwise re-deriving. They change together with reac.discovery.seq, so a reader that
 * trusts the seq gets a consistent set. */
#define REAC_PROP_MASTER_STATE  "reac.master.state"   /* us | foreign | none            */
#define REAC_PROP_MASTER_MAC    "reac.master.mac"     /* the driving master, or "none"  */
#define REAC_PROP_PACE_SOURCE   "reac.pace.source"    /* who owns the WIRE pace         */
/* A foreign master is live while WE are established — reported, never acted on (spec §6 Q1:
 * yielding drops a box mid-audio, holding breaks the one-master law, and the choice is the
 * operator's). "1" or "0". */
#define REAC_PROP_MASTER_CONFLICT "reac.master.conflict"
/* WHAT a rival master is, by its frame GEOMETRY, and why it was not joined (§2b). A desk is
 * joined and carries no refusal; a stagebox strapped to master claims master while emitting a
 * box width, and only the length tells them apart. `none` when there is no rival. */
#define REAC_PROP_RIVAL_KIND    "reac.master.rival.kind"  /* none | desk | box | unknown */
#define REAC_PROP_REFUSAL       "reac.master.refusal"     /* none | rival-master-{box,unknown} */

/* ---- HEALTH (workstream CLK, 2026-08-23) --------------------------------- *
 *
 * WHY THESE ARE NODE PROPERTIES AND NOT A NEW WIRE. reac-pw already speaks to the
 * console through exactly one doorway — the properties on its own PipeWire nodes,
 * which is where reac.link-state, reac.box-model and reac.pace.source already
 * live. Adding a socket, a file or an HTTP endpoint would be a second ledger for
 * the same facts, with neither door announcing the other. One store, one writer.
 *
 * The console side is NOT built here (another lane owns that repo). openmixer's
 * telemetry is a settled design — its own SSE, latest-wins, skip when late, never
 * accumulate — and these rows are shaped to drop straight into it: every one is a
 * scalar over the window that just closed, so a consumer that misses three
 * updates has lost nothing but resolution.
 *
 * All values are decimal strings; the units are in the names or stated here. */
#define REAC_PROP_HEALTH_DRIFT_PPM   "reac.health.drift-ppm"
	/* Transmit deficit over the last window: (nominal - emitted) / nominal, in
	 * ppm. POSITIVE means fewer frames reached the wire than the rate asks for,
	 * so the TX ring grows and the depth guard will eventually discard. This is
	 * THE number: on the live rig, unfixed, it reads about +900. */
#define REAC_PROP_HEALTH_DISCARD_FPS "reac.health.discard-fps"
	/* Frames the depth guard discarded per second. Nonzero means audio is being
	 * dropped RIGHT NOW, in 64 ms blocks, with no xrun and no other symptom. */
#define REAC_PROP_HEALTH_DISCARD_MS  "reac.health.discard-ms-per-s"
	/* The same loss restated as what an operator hears: milliseconds of audio
	 * lost per second. */
#define REAC_PROP_HEALTH_TX_ERRORS   "reac.health.tx-errors"
	/* Cumulative sendto() failures (EAGAIN on a backed-up NIC queue). Each one is
	 * a frame built, counter-stamped and never sent. Difference two readings. */
#define REAC_PROP_HEALTH_LATE_WAKES  "reac.health.late-wakes"
	/* Cumulative slots where the pacer woke more than a full period late. */
#define REAC_PROP_HEALTH_LATE_PS     "reac.health.late-wakes-per-s"
#define REAC_PROP_HEALTH_CATCHUP_PS  "reac.health.catchup-slots-per-s"
	/* Overslept slots REPAID on the grid per second. The correction working,
	 * which is the thing an operator should be able to watch rather than trust. */
#define REAC_PROP_HEALTH_DROPPED_PS  "reac.health.dropped-slots-per-s"
	/* Overslept slots ABANDONED per second: the debt exceeded the catch-up
	 * budget, so it was declared instead of smeared onto the wire. */
#define REAC_PROP_HEALTH_DEBT_MAX    "reac.health.slot-debt-max"
	/* Largest SINGLE overslept debt in the window, in slots. At or below the
	 * catch-up budget every miss was repayable; above it is the tail that a short
	 * run cannot show, and the number that says whether the budget is set right. */
#define REAC_PROP_HEALTH_RING_FRAMES "reac.health.ring-frames"
#define REAC_PROP_HEALTH_RING_MS     "reac.health.ring-ms"
	/* TX ring depth at the close of the window, in frames and in ms of
	 * graph->wire latency. */
#define REAC_PROP_HEALTH_RATE_MATCH  "reac.health.rate-match-ppm"
	/* The correction currently handed to PipeWire's resampler via io_rate_match,
	 * in ppm. "n/a" when the graph gave this node no rate-match area (no
	 * resampler on the link, so there is nothing to steer). */

/* reac-pw only ever LISTENS: it reports frames its promiscuous socket already receives
 * and transmits nothing to discover. There is deliberately no "probing" value — active
 * probing a live segment could disturb a joined box. */
#define REAC_DISCO_STATE_LISTENING "listening"

/* Values reac.link-state is stamped with (reac_link_state_name below). */
enum reac_link_state {
	REAC_LINK_PROBING = 0,
	REAC_LINK_GRANTING,
	REAC_LINK_ESTABLISHED,
	REAC_LINK_DROPPED,
};

/* PURE: map the master FSM state (+ the caller's "did a drop just happen"
 * event flag) to the badge vocabulary. Pass just_dropped=0 to read the state
 * mapping alone; pass 1 to get the one-shot DROPPED overlay regardless of
 * `st` (the caller only ever sets it true on the drain cycle a backward
 * transition was observed, at which point `st` has already settled back to
 * PROBING — see the module comment). */
enum reac_link_state reac_link_state_from_master(enum reac_master_state st,
                                                  int just_dropped);

/* The exact string stamped into REAC_PROP_LINK_STATE. Never NULL. */
const char *reac_link_state_name(enum reac_link_state s);

/* A destination for one published node property: the key and the value it takes.
 *
 * IT EXISTS SO THE STAMP CAN BE TESTED. Composing the string and writing it onto
 * the node are one act, and the act is what a consumer reads — a formatter tested
 * on its own leaves "which key, with what value, and is it written at all" as the
 * untested half, which is exactly the half that was wrong (the badge set named the
 * box five ways and never by its address). pw_properties lives behind libpipewire
 * and no unit test here links it, so the sink and the source pass their own
 * one-line pw_properties_set adapter and a test passes a recording fake. */
typedef void (*reac_prop_set_fn)(void *ctx, const char *key, const char *value);

/* Compose REAC_PROP_BOX_MAC from a MAC packed by reac_mac48_pack (reac_mac.h) and
 * STAMP it through `set`: "aa:bb:cc:dd:ee:ff" lowercase, or REAC_BOX_MAC_NONE when
 * `mac48` is 0. Zero is the no-MAC sentinel the pack function already defines (no
 * device carries the all-zero address) and it is what the master holds while it
 * has no box, so "no box" and "a box at 00:00:00:00:00:00" cannot collide.
 *
 * ALWAYS STAMPS, present or not. pw_properties_update MERGES, so a key left
 * unwritten keeps the DEPARTED box's address and a consumer goes on naming a
 * chassis that has left the wire. `set` NULL is a no-op. */
void reac_box_mac_publish(uint64_t mac48, reac_prop_set_fn set, void *ctx);

#endif /* REAC_LINK_STATE_H */
