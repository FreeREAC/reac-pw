// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_role_cfg — the `reac.cfg.role` live control, the ROLE half of runtime
 * config alongside reac_rate_cfg's pace half
 * (docs/design/specs/2026-08-26-reac-runtime-config.md, in the openmixer tree).
 *
 * THE WRITE-SIDE PROP NAME AND ENCODING ARE ALREADY SHARED VOCABULARY. Unlike
 * rate (where reac-pw's own module came first and libreac's reac_cfg.h caught
 * up afterward), openmixer's side got here first: libreac's
 * include/reac/reac_cfg.h (checked out beside this repo) already declares
 *
 *     #define REAC_CFG_ROLE_PROP   "reac.cfg.role"
 *     #define REAC_CFG_ROLE_MASTER 0
 *     #define REAC_CFG_ROLE_SLAVE  1
 *
 * — a NUMERIC flag, the same two-state convention `reac.headamp.<ch>.phantom`
 * already uses, NOT the "master"/"slave" strings reac_role.h's --role CLI
 * argument takes. REAC_CFG_PROP_ROLE / REAC_CFG_ROLE_VALUE_MASTER / _SLAVE
 * below match those three byte-for-byte (verified against the checked-out
 * header, 2026-08-26); this module conforms to an already-settled wire shape
 * rather than inventing one.
 *
 * THE ANSWER SIDE IS NOT YET SHARED VOCABULARY. reac_rate_cfg's own answer
 * props (reac.cfg.rate.state / .refused) were implemented here first and only
 * mirrored into libreac + openmixer's TS afterward — this module follows that
 * same order. REAC_PROP_ROLE / REAC_PROP_ROLE_STATE / REAC_PROP_ROLE_REFUSED
 * and the REAC_ROLE_STATE_* strings are LOCAL to reac-pw until a later
 * increment mirrors them the way rate's were (see this repo's own history:
 * effe386 landed reac_rate_cfg locally, libreac's 6b9b0f3/1aaff657 unified it
 * afterward).
 *
 * HONESTY (spec §1: an unfinished actuation is a state, never a silent
 * success). A rate change re-establishes INSIDE one running engine
 * (reac_pacer_apply_rate re-runs the same FSM init at a new fps, in place). A
 * ROLE change is a different order of invasiveness: master and slave are TWO
 * DIFFERENT ENGINES (reac_sink_node + reac_pacer vs. reac_slave — see
 * main.c's listener_open), each opening its own AF_PACKET socket, claiming or
 * not claiming the segment lock, and running its own thread. So the work
 * splits in two, and THIS MODULE IS ONLY THE FIRST HALF:
 *
 *   - the DECISION core lives here — parse, same-role-is-a-no-op detection, and
 *     the answer a request publishes when nothing is carrying it out. Pure, and
 *     fully unit-tested (tests/test_reac_role_cfg.c);
 *   - the SWAP and its LIFECYCLE live in reac_role_swap.h. main.c performs the
 *     change as a clean listener close + open in the other engine, and the
 *     answer is DERIVED on every publish from the segment's own record plus the
 *     running engine's state — never frozen at the moment the assertion was
 *     parsed, because the node that parsed it is destroyed by the very swap it
 *     accepted.
 *
 * reac_role_cfg_apply_state below is therefore the answer for a caller with NO
 * segment record behind it (a unit harness): a same-role assertion is already
 * the fact and reads `applied`, a role-CHANGING one reads
 * REAC_ROLE_STATE_REESTABLISH_PENDING and stays there, because nothing in this
 * module moves it further. Never a silent no-op, never a fake success.
 *
 * WHAT REMAINS UNPROVEN, on either side of that split: that a real Roland box
 * or desk RE-ATTACHES across a live swap. No capture in reac-captures shows a
 * desk ceding a segment or a box under a master that changes role, so the
 * daemon's own transcript is evidence about OUR engines and about nothing else.
 * That half is an operator-present rig test.
 *
 * PURE decision core, no I/O, no PipeWire, no engine touched — same shape as
 * reac_rate_cfg and reac_headamp_prop: unit-testable offline. Two callers:
 * reac_sink_node's param_changed (the master's door, where `reac.cfg.rate` is
 * read) and reac_source_node's (a slave's only door — it has no playback
 * node). */
#ifndef REAC_ROLE_CFG_H
#define REAC_ROLE_CFG_H

#include <stddef.h>

#include <reac/reac_role.h>

struct spa_pod;

/* The cfg namespace key this increment adds. Byte-for-byte libreac's
 * REAC_CFG_ROLE_PROP. */
#define REAC_CFG_PROP_ROLE "reac.cfg.role"

/* The wire ENCODING for reac.cfg.role: SPA Int (or Float, same tolerance as
 * rate) 0 or 1 — NOT the "master"/"slave" strings reac_role_parse takes for
 * --role. Byte-for-byte libreac's REAC_CFG_ROLE_MASTER / REAC_CFG_ROLE_SLAVE. */
#define REAC_CFG_ROLE_VALUE_MASTER 0
#define REAC_CFG_ROLE_VALUE_SLAVE  1

/* Published, read-side props (see this header's own "ANSWER SIDE" note: local
 * to reac-pw for now). "none" is this codebase's established sentinel for "no
 * value applies" (see reac.master.mac in reac_link_state.h). */
#define REAC_PROP_ROLE         "reac.role"              /* "0" | "1": the RUNNING role */
#define REAC_PROP_ROLE_STATE   "reac.cfg.role.state"     /* applied | reestablish-pending */
#define REAC_PROP_ROLE_REFUSED "reac.cfg.role.refused"   /* code, or "none"     */

/* A same-role assertion is genuinely, immediately true: nothing needed to
 * change, so nothing is left undone. */
#define REAC_ROLE_STATE_APPLIED "applied"
/* The answer while the cross-engine swap is OWED or IN FLIGHT: the request is
 * well-formed and accepted, and either nothing owns the segment right now or the
 * engine that does is not the one asked for. Published INSTEAD OF ever claiming
 * "applied" for a change that has not finished. reac_role_swap_state is what
 * moves it on, and only when the NEW engine is performing its role — for a
 * slave that means enrolled by a desk, which on a quiet wire never happens and
 * reads REAC_ROLE_STATE_HUNTING for as long as the hunt lasts. */
#define REAC_ROLE_STATE_REESTABLISH_PENDING "role_reestablish_pending"

/* Why a `reac.cfg.role` assertion was refused. REFUSE_NONE doubles as the
 * published state once a refusal is superseded by an accepted role. There is
 * only one refusal today: role has no drivability-style constraint the way
 * rate does (every well-formed role value is a value we can, in principle,
 * become) — a future increment that DOES bar a transition (e.g.
 * reac_role_validate's "slave needs --tx") has exactly this enum to extend. */
enum reac_role_refuse {
	REAC_ROLE_REFUSE_NONE = 0,
	REAC_ROLE_REFUSE_MALFORMED,   /* the prop value was not 0 or 1 */
};

/* Short code for REAC_PROP_ROLE_REFUSED; "none" when nothing is refused. */
const char *reac_role_refuse_code(enum reac_role_refuse r);

/* Does applying requested_role on a daemon currently running current_role
 * require the invasive cross-engine swap (non-zero), or is it a same-role
 * no-op republish (zero)? */
int reac_role_cfg_changes(enum reac_role current_role, enum reac_role requested_role);

/* THE decision: what state a `reac.cfg.role` assertion of requested_role
 * publishes, for a daemon currently running current_role. Pure — makes no
 * engine change at all (see this header's HONESTY note); the caller stores
 * this string verbatim as the answer to reac.cfg.role.state. */
const char *reac_role_cfg_apply_state(enum reac_role current_role,
                                      enum reac_role requested_role);

/* Parse a `reac.cfg.role` entry out of a SPA_PARAM_Props object pod's
 * SPA_PROP_params list (the same carrier reac_rate_prop_parse and
 * reac_headamp_prop_parse read). Accepted as Int or Float (same tolerance as
 * rate), and the numeric value must be exactly REAC_CFG_ROLE_VALUE_MASTER or
 * REAC_CFG_ROLE_VALUE_SLAVE. Returns 1 and sets *out_role if the key was
 * present and its value was one of the two legal flags, 0 if the key is
 * simply absent (a Props write that only touched volume, head-amp or rate),
 * -1 if the key was present but its value was not readable as a number or
 * was neither 0 nor 1, or if `props` is not an Object pod at all. PURE: no
 * PipeWire, no engine — unit-testable offline. */
int reac_role_prop_parse(const struct spa_pod *props, enum reac_role *out_role);

#endif /* REAC_ROLE_CFG_H */
