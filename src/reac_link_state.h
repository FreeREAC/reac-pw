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

#include "reac_master.h"

/* PipeWire node property keys the badge consumer reads. reac.link-state is
 * the must-have; reac.box-model / reac.box-width are best-effort (populated
 * once the master's box recognizer matches a fixed-matrix model; "none" /
 * "0x0" until then — see reac_ctrl.h's reac_box_model). */
#define REAC_PROP_LINK_STATE "reac.link-state"
#define REAC_PROP_BOX_MODEL  "reac.box-model"
#define REAC_PROP_BOX_WIDTH  "reac.box-width"

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

#endif /* REAC_LINK_STATE_H */
