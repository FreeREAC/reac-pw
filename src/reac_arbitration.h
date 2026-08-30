/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_arbitration — WHO DRIVES THIS SEGMENT, as a published fact.
 *
 * REAC keeps ONE master per segment, and until now nothing anywhere modelled that as a
 * first-class thing. Plugging an S-4000S in slave mode (2026-08-20) produced a sighting of
 * role `master`, a model that never resolved, and a reac-pw that probed forever, because the
 * segment's master topology existed only as scattered evidence nobody added up.
 *
 * This is increment 2 of `docs/design/specs/2026-08-20-reac-master-arbitration.md`: compute the
 * segment aggregate from the sightings the discovery table already holds plus our own FSM
 * state, and publish it. **PASSIVE — it decides nothing.** Joining a foreign master (§2) and
 * promoting the clock with none (§3) are later increments; this one exists so both can be built
 * against a fact rather than re-derived, and so an operator can SEE the topology that is
 * currently only inferable from a log.
 *
 * ARBITRATION KEYS ONLY ON UNAMBIGUOUS MASTER EVIDENCE. The catch-all sighting buckets never
 * feed it: the S-4000S incident IS a box misfiled through a catch-all, and the cost of acting
 * on that misfile is a dead segment. A sighting of UNKNOWN role stays a sighting — it is
 * neither promoted to a master nor counted as its absence.
 *
 * THE REAC CLOCK IS NOT THE PIPEWIRE CLOCK. `pace_source` names who owns the WIRE's
 * transmission pace and nothing else. The graph keeps its own clock whoever masters the
 * segment; reac-pw is the boundary and rate-matches across it. No arbitration outcome ever
 * re-parents the graph clock.
 */
#ifndef REAC_ARBITRATION_H
#define REAC_ARBITRATION_H

#include "reac_disco.h"
#include "reac_master.h"   /* enum reac_master_state: the FSM's OWN state */

#include <stdint.h>

/**
 * Who drives this segment.
 *
 * Distinct from `enum reac_master_state`, which is our FSM's own progress
 * (IDLE/PROBING/GRANTING/ESTABLISHED). That says what WE are doing; this says who OWNS the
 * wire. Both are needed and conflating them is how "we are probing" came to be read as "there
 * is no other master".
 */
enum reac_segment_master {
	/** The wire holds no master evidence at all. */
	REAC_SEGMENT_NONE = 0,
	/** Our FSM is established, or probing unopposed. */
	REAC_SEGMENT_US,
	/** An unambiguous OTHER master lives on this segment. */
	REAC_SEGMENT_FOREIGN,
};

/** Who owns the REAC pace — the speed of transmission on the wire — right now. */
enum reac_pace_source {
	/** We time the stream off CLOCK_MONOTONIC, disciplined by nothing. */
	REAC_PACE_FREE_RUN = 0,
	/** A foreign master times the stream and we recover its pace. */
	REAC_PACE_FOREIGN_MASTER,
	/** We time it, disciplined to a NIC/external PHC. */
	REAC_PACE_PHC,
	/** We time it, disciplined to a hardware-driven PipeWire graph clock as a FREQUENCY
	 *  reference. Never means the two domains merged. */
	REAC_PACE_GRAPH_REF,
	/** We time it, disciplined to the box's own counter slope. */
	REAC_PACE_BOX_SLOPE,
};

/** What a rival master turned out to be. The strings are the published prop values. */
enum reac_rival_kind {
	REAC_RIVAL_NONE = 0,
	/** The 40-channel downstream — a real desk. Joinable per §2. */
	REAC_RIVAL_DESK,
	/** A box width from something claiming master: a stagebox in the wrong mode. REFUSE. */
	REAC_RIVAL_BOX,
	/** No legal geometry heard yet. Refused too — §4's catch-all conservatism: a frame kind
	 *  nobody has captured must not flip the segment's topology. */
	REAC_RIVAL_UNKNOWN,
};

/** The segment aggregate, as the props carry it. */
struct reac_arbitration {
	enum reac_segment_master state;
	/** The driving master's MAC; meaningful only when {@link have_mac} is 1. */
	uint8_t mac[6];
	int have_mac;
	/** Who owns the wire pace. */
	enum reac_pace_source pace;
	/**
	 * A foreign master is live WHILE WE ARE ESTABLISHED — the mid-flight conflict.
	 *
	 * Reported rather than acted on: yielding drops a box mid-audio, holding breaks the
	 * one-master law, and which of those is right is the operator's call (spec §6 Q1). Until
	 * it is answered the rule is hold and report LOUDLY, and this flag is that report. It is
	 * deliberately separate from {@link state}, which stays `us` because we are in fact still
	 * driving — a surface that showed `foreign` here would say the desk had taken over when
	 * it has not.
	 */
	int conflict;
	/**
	 * WHAT THE RIVAL IS, decided by its frame GEOMETRY rather than by its control frames
	 * (spec §2b). A desk drives with the 40-channel downstream; a stagebox emits its own,
	 * smaller declared width — and a stagebox strapped to master mode claims master while
	 * emitting a box geometry. The two want opposite responses, so they cannot share a name.
	 * REAC_RIVAL_NONE when there is no rival at all.
	 */
	enum reac_rival_kind rival;
};


/**
 * Classify a rival by the width its frames carry.
 *
 * 40 channels is the master downstream and nothing else is; every smaller legal geometry is a
 * box upstream of that width. 0 means no legal `52 + n*36` frame has been heard from the peer,
 * which is UNKNOWN rather than narrow — and unknown is refused, per §4's rule that a frame kind
 * nobody has captured must not flip the segment's topology.
 */
enum reac_rival_kind reac_rival_kind_from_channels(unsigned channels);

/** Wire name for a rival kind — `none` | `desk` | `box` | `unknown`. */
const char *reac_rival_kind_name(enum reac_rival_kind k);

/** The refusal code for a rival kind, or `"none"` when nothing is refused (no rival, or a
 *  desk, which is JOINED rather than refused). */
const char *reac_rival_refusal(enum reac_rival_kind k);

/** Wire names, stable across versions — these strings ARE the published prop values. */
const char *reac_segment_master_name(enum reac_segment_master s);
const char *reac_pace_source_name(enum reac_pace_source p);

/**
 * Compute the segment aggregate.
 *
 * `fsm` is our own FSM's answer about itself, taken as its existing enum rather than as
 * booleans so there is one vocabulary for it. `now_ns` ages the sightings: a master heard once
 * and gone is not a master, and the table's own staleness bar is what decides that, so a
 * segment whose desk was unplugged stops reporting it.
 *
 * Pure: no clock of its own, no I/O, no state kept between calls.
 */
void reac_arbitrate(const struct reac_disco_table *table,
                    const uint8_t our_mac[6],
                    enum reac_master_state fsm,
                    enum reac_pace_source own_pace,
                    uint64_t now_ns,
                    struct reac_arbitration *out);

#endif /* REAC_ARBITRATION_H */
