// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_role_swap — what a segment ANSWERS while a `reac.cfg.role` change is
 * being carried out, and once it has been
 * (docs/design/specs/2026-08-20-reac-master-arbitration.md §8, in the openmixer
 * tree). reac_role_cfg decides an assertion; this module answers for it
 * afterwards.
 *
 * THE TWO ARE SEPARATE BECAUSE A ROLE CHANGE IS NOT AN INSTANT. Master and
 * slave are two different engines — main.c's listener_open branches on the role
 * and builds reac_sink_node + reac_pacer for one and reac_slave for the other,
 * each with its own AF_PACKET socket, its own thread, and (master only) the
 * segment lock. Carrying the change out means closing one and opening the other,
 * so the segment passes through a window in which NOTHING owns it. A single
 * "applied/pending" pair cannot describe that window; this module's job is that
 * it never has to be described by guessing.
 *
 * WHAT "PERFORMING THE ROLE" MEANS IS DIFFERENT PER ROLE, and that asymmetry is
 * the reason this module exists at all:
 *
 *   - A MASTER performs its role the moment its pacer drives the wire. A master
 *     with nothing plugged in is still mastering: it holds the segment lock, it
 *     paces, it probes, and a box that appears will be granted. REAC_M_PROBING
 *     and beyond is therefore the role FULFILLED, not a half-state
 *     (REAC_M_IDLE is the sub-millisecond transient before the first emitted
 *     frame — #130).
 *   - A SLAVE performs its role only once a master has ENROLLED it. A slave on a
 *     quiet wire floods, then cold-connects on the retry grid, and keeps
 *     cold-connecting for as long as the cable stays silent (reac_fsm's
 *     FLOOD_ANNOUNCE -> COLDCONNECT). That hunt is the honest terminal state of a
 *     recorder waiting for a desk that is not there. Answering "applied" for it
 *     would tell an operator the console had joined a desk it has never heard.
 *
 * So the published vocabulary needs a third member beside applied and pending,
 * and REAC_ROLE_STATE_HUNTING is it.
 *
 * PURE decision core: no PipeWire, no socket, no thread, no engine touched —
 * the same shape as reac_role_cfg, reac_rate_cfg and reac_link_state, and
 * unit-testable offline (tests/test_reac_role_swap.c). The record it keeps is
 * MAIN-LOOP-ONLY state: every writer (the props door, the re-open, the badge
 * publish) already runs there, so it carries no atomics and needs none.
 *
 * WHAT THIS MODULE CANNOT CLAIM. It answers for the swap the daemon PERFORMS;
 * it says nothing about whether a real Roland box or desk re-attaches across
 * one. No capture in reac-captures shows a desk ceding a segment or a box under
 * a master that changes role, so the re-attach is unproven on hardware and is
 * an operator-present rig test (arbitration spec §8's own ordered work, item 1).
 * The states below describe OUR engines truthfully; a peer's reaction to them is
 * outside what any offline test here can establish. */
#ifndef REAC_ROLE_SWAP_H
#define REAC_ROLE_SWAP_H

#include <reac/reac_role.h>
#include "reac_role_cfg.h"  /* the answer vocabulary this one extends (see below) */
#include <reac/reac_master.h>   /* enum reac_master_state — the master engine's FSM */

/* The answer strings published on REAC_PROP_ROLE_STATE (reac_role_cfg.h). The
 * first two are that header's already-declared vocabulary, repeated by include
 * rather than by copy; HUNTING is this module's addition and, like the rest of
 * the answer side, is LOCAL to reac-pw until a later increment mirrors it into
 * libreac's reac_cfg.h the way rate's answer props were (reac_role_cfg.h's
 * "ANSWER SIDE" note). */
#define REAC_ROLE_STATE_HUNTING "role_hunting"

/* How far the engine that currently owns the segment has got with the job its
 * role names. Derived from that engine's own state, never stored. */
enum reac_role_engine {
	/* Nothing owns the segment: mid-swap, or an engine that failed to open. */
	REAC_ROLE_ENGINE_DOWN = 0,
	/* The engine runs, and the role is not being performed on the wire yet —
	 * a slave that no master has enrolled. */
	REAC_ROLE_ENGINE_HUNTING,
	/* The engine is doing its role's own job. */
	REAC_ROLE_ENGINE_PERFORMING,
};

/* One segment's role lifecycle. `asserted` is what the console asked us to be
 * (or, until it asks, the role we booted in); `running` is whose engine last
 * came up here. They differ exactly while a swap is owed or has failed. */
struct reac_role_swap {
	enum reac_role asserted;
	enum reac_role running;
	int            engine_up;   /* 1 while `running`'s engine owns the segment */
};

/* Seed the record from the role this segment boots in. Nothing is owed and
 * nothing is up yet — reac_role_swap_opened() records the engine. */
void reac_role_swap_init(struct reac_role_swap *s, enum reac_role boot_role);

/* Record a `reac.cfg.role` assertion of `want`. Returns non-zero when the
 * cross-engine swap must run (the caller stashes the re-open), 0 when the
 * assertion names the role already running — the existing no-op, which changes
 * nothing and is answered as the fact it already is. */
int reac_role_swap_request(struct reac_role_swap *s, enum reac_role want);

/* The listener opened in `role`: that engine now owns the segment. */
void reac_role_swap_opened(struct reac_role_swap *s, enum reac_role role);

/* The listener closed: the socket, the thread and (for a master) the segment
 * lock are gone, and nothing owns the segment until the next open. */
void reac_role_swap_closed(struct reac_role_swap *s);

/* The master engine's contribution to the answer: its pacer drives the wire
 * from its first frame, so PROBING and beyond is the role performed. */
enum reac_role_engine reac_role_engine_of_master(int engine_up, enum reac_master_state fsm);

/* The slave engine's contribution. `established` is reac_slave's own atomic
 * ESTABLISHED flag — the one slave reading that crosses the engine thread with
 * no torn read, and precisely the fact the answer turns on: enrolled by a
 * master, or still hunting for one. */
enum reac_role_engine reac_role_engine_of_slave(int engine_up, int established);

/* RETIRED 2026-09-09 (0.5.6). This answered the role for a RECEIVE-ONLY join — 0.5.2's
 * rule that a segment which merely decodes a box master's broadcast IS the slave role
 * performed. There is no receive-only join any more: a wire a box masters is joined by the
 * same reac_slave engine a desk-mastered wire is, so its role is answered by
 * reac_role_engine_of_slave like every other slave, and its link-state by that engine's own
 * ESTABLISHED flag. Kept out of the header deliberately rather than left dead: the rule it
 * encoded is the one the operator overturned ("S-0808 is not enrolled but omx sees it
 * available"), and a predicate that still answers it is a second opinion waiting to be
 * called. */
/* THE ANSWER for REAC_PROP_ROLE_STATE, given the record and what the live
 * engine reports. Honest in every window:
 *
 *   nothing owns the segment            -> role_reestablish_pending
 *   the engine up is not the one asked  -> role_reestablish_pending
 *   the asked-for engine, still hunting -> role_hunting
 *   the asked-for engine, performing    -> applied
 *
 * `applied` is reached only through the last line, so a swap can never be
 * reported as done by anything but the NEW engine getting on with the job. */
const char *reac_role_swap_state(const struct reac_role_swap *s,
                                 enum reac_role_engine engine);

/* Does a segment in `role` emit head-amp control? MASTER ONLY. A box is TOLD
 * what its preamps do, it never tells its desk — reac_slave's whole emit
 * vocabulary (enum reac_slave_emit) has no head-amp member, so this is true by
 * construction. The predicate exists so a swap can ASSERT the property rather
 * than leave it as one nobody re-checks once the engines have changed places. */
int reac_role_emits_headamp(enum reac_role role);

#endif /* REAC_ROLE_SWAP_H */
