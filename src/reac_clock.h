// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_clock — pure clock-discipline core: lock-source selection + the period DLL.
 *
 * THREE WORDS THAT ARE NOT SYNONYMS (issue #75). Conflating them is the error this
 * module exists to remove, so nothing below is allowed to blur them:
 *
 *   ROLE             master (we act as the desk) or slave (we act as a stagebox).
 *                    A --role argument; see reac_role.h.
 *   PACE             who DETERMINES the wire cadence. In the master role we
 *                    generate it — pace is an OUTPUT of this process. In the slave
 *                    role the external master generates it — pace is an INPUT we
 *                    must align to.
 *   CLOCK REFERENCE  what that cadence is DERIVED FROM or DISCIPLINED TO. It is
 *                    not the role and it is not the pace.
 *
 * The rate is a fourth, independent thing: a number chosen at the master (on a
 * Roland desk, the REAC menu; `--rate` is our equivalent). It picks the nominal
 * frame period and nothing else. Rate is NOT clock: the clock can be the mixer's
 * internal oscillator, a stagebox on the segment, or an external word-clock source
 * (Roland desks carry the connector; that is how a REAC rig joins a house clock
 * shared with other digital gear). There is EXACTLY ONE clock master on a network
 * — two devices both claiming it is the classic cause of clicks, dropouts, and
 * links that come and go.
 *
 * BEING THE REAC MASTER DOES NOT MAKE US THE CLOCK MASTER. Driving the handshake
 * says nothing about which oscillator the rig is disciplined to.
 *
 * THE HIERARCHY IS ROLE-DEPENDENT — two distinct orderings, not one list with the
 * role as a tiebreak, because the clock relationship is INVERTED between the roles:
 *
 *   MASTER (we generate the pace; it is an OUTPUT, and it should be disciplined to
 *   a reference from elsewhere). This is the gap this module closes:
 *       1. PHC     an external / NIC hardware clock (PTP, or a word-clock-
 *                  disciplined NIC) — the only reference independent of both the
 *                  host and the REAC segment.
 *       2. GRAPH   the PipeWire graph clock, and ONLY when the graph is driven by
 *                  locked hardware (reac_clock_name_is_hardware).
 *       3. BOX     the peer box's own counter slope, which reac_rx already
 *                  measures. Ranked LAST of the references for a reason: a box
 *                  that recovers its word clock from OUR cadence hands our own
 *                  pace back, so the measurement degenerates to ~0 error and the
 *                  loop is a harmless no-op. It is a GENUINE reference only in the
 *                  case the issue names — a stagebox on the segment that is itself
 *                  the clock master (fed from a house word clock).
 *       4. FREERUN the host monotonic clock. Not a lock. We say so.
 *
 *   SLAVE (the master generates the pace; it is an INPUT):
 *       1. WIRE    the master's cadence on the wire. Non-negotiable, top of the
 *                  hierarchy, nothing outranks it — a PHC or a locked graph clock
 *                  in the same host does NOT displace it.
 *
 * SCOPE, stated plainly. The slave path is ALREADY CORRECT BY CONSTRUCTION and is
 * NOT touched by this work: reac_slave runs no clock_nanosleep pacer at all; every
 * received master frame is one tick and we emit exactly one upstream frame in
 * response, so frame arrival IS the slot clock (see the reac_slave.h/.c contracts).
 * There is nothing to discipline there and nothing here to wrap around it. The
 * slave row exists so that REPORTING can name the reference that path is already
 * locked to, and so that the master hierarchy can never be applied to a slave by
 * accident. Only the MASTER path is wired to a DLL.
 *
 * This module is PURE: ppm in, period out. No I/O, no globals, no clock_gettime,
 * no knowledge of sockets or PipeWire. Everything below is unit-tested offline;
 * the wiring that feeds it lives in reac_pacer / reac_sink_node.
 */
#ifndef REAC_CLOCK_H
#define REAC_CLOCK_H

#include <stdint.h>
#include <stddef.h>

#include "reac_role.h"

enum reac_clock_source {
	REAC_CLOCK_SRC_FREERUN = 0,  /* the host monotonic clock — the honest last resort */
	REAC_CLOCK_SRC_BOX,          /* the peer box's counter slope (reac_rx measures it) */
	REAC_CLOCK_SRC_GRAPH,        /* the PipeWire graph clock, hardware-driven only    */
	REAC_CLOCK_SRC_PHC,          /* external / NIC hardware clock                     */
	REAC_CLOCK_SRC_WIRE,         /* the master's cadence (slave role — reporting only) */
};
#define REAC_CLOCK_SRC_COUNT 5

#define REAC_CLOCK_AVAIL_BOX    (1u << REAC_CLOCK_SRC_BOX)
#define REAC_CLOCK_AVAIL_GRAPH  (1u << REAC_CLOCK_SRC_GRAPH)
#define REAC_CLOCK_AVAIL_PHC    (1u << REAC_CLOCK_SRC_PHC)
#define REAC_CLOCK_AVAIL_WIRE   (1u << REAC_CLOCK_SRC_WIRE)

/* The role's ordered hierarchy, best first. FREERUN is never in the list — it is
 * what "none of these" means. Exposed so the ordering can be asserted rather than
 * inferred from the selector's branches. */
const enum reac_clock_source *reac_clock_hierarchy(enum reac_role role, int *n);

/* Best available reference for a role. PURE. Availability bits for sources OUTSIDE
 * the role's hierarchy are IGNORED — a slave offered a NIC PHC still locks to the
 * wire, because for a slave the wire is not one candidate among several. An empty
 * (or entirely out-of-role) bitmap selects FREERUN. */
enum reac_clock_source reac_clock_select(enum reac_role role, uint32_t avail);

/* The reference's human name ("NIC/external PHC", "master cadence", ...). Never
 * printed without the role and the pace beside it (see reac_clock_disc_describe):
 * the transcript must say who determines the cadence AND what it is disciplined
 * to, never just one of the two. */
const char *reac_clock_source_name(enum reac_clock_source s);

/* ---- the DLL ------------------------------------------------------------- *
 * Input: the ppm error of the reference against OUR emission cadence's clock —
 * exactly the quantity reac_rx already publishes (positive = the reference runs
 * FAST versus CLOCK_MONOTONIC, so we must emit faster, i.e. shorten the period).
 * Output: the slot period to sleep to.
 *
 * The loop is a single-pole integrator over the frequency error:
 *
 *     applied += KP * (measured - applied)
 *
 * The measurement IS a frequency offset, not an accumulating phase error, so one
 * pole is the whole loop: it converges monotonically for 0 < KP < 1 (no overshoot,
 * no second pole to ring), and its fixed point is applied == measured, i.e. zero
 * steady-state error. `applied` is the loop's memory — that is what makes holdover
 * free: freeze it and the cadence keeps the last good rate.
 *
 * PHASE IS NEVER STEPPED. The only thing this module ever changes is the LENGTH of
 * the next period; the pacer's absolute deadline still advances by exactly one
 * period every slot. A phase step is an audible click; a slow pull is inaudible.
 *
 * THREE BOUNDS so a bad or noisy reference cannot run the cadence away:
 *   SANE  a measurement beyond this is not a clock error, it is garbage (a
 *         restarted counter, a stalled graph, a mismatched rate). It is REJECTED —
 *         not clamped in — and counted, so a wild reference contributes nothing at
 *         all rather than saturating the loop.
 *   SLEW  the applied correction may move at most this much per update, so even a
 *         step in the measurement is followed as a glide.
 *   MAX   the applied correction is hard-clamped. Real audio clocks live inside
 *         +-100 ppm; +-200 ppm is generous headroom and still a period no box can
 *         mistake for a rate change.
 */
#define REAC_DLL_KP            0.25   /* single-pole gain (0 < KP < 1)          */
#define REAC_DLL_MAX_PPM      200.0   /* hard clamp on the applied correction   */
#define REAC_DLL_SLEW_PPM      10.0   /* max applied change per update          */
#define REAC_DLL_SANE_PPM    2000.0   /* beyond this a sample is garbage        */
#define REAC_DLL_LOCK_PPM       2.0   /* FILTERED residual that counts as tracking */
#define REAC_DLL_LOCK_UPDATES     4   /* consecutive in-band updates -> LOCKED  */
#define REAC_DLL_RESID_ALPHA   0.10   /* EMA over |residual| (~10 updates)      */

struct reac_dll {
	long   nominal_ns;      /* the rate's period — never modified by the loop  */
	double applied_ppm;     /* the loop's memory (and the holdover value)      */
	double last_residual;   /* measured - applied, at the last accepted update */
	/* EMA of |residual|. The LOCK decision is made on THIS, not on the last
	 * sample: a real reference carries a few ppm of per-window measurement noise
	 * (reac_rx recomputes its slope ~4x/s), so an instantaneous test would flap
	 * between locked and locking forever while the loop is in fact tracking
	 * perfectly. Seeded to the first residual so a fresh loop can never read as
	 * in-band before it has measured anything. */
	double resid_ema;
	uint64_t updates;       /* accepted samples                               */
	uint64_t rejected;      /* samples outside SANE                           */
	uint64_t clamped;       /* updates whose target hit MAX                   */
};

void   reac_dll_init(struct reac_dll *d, long nominal_period_ns);
/* Feed one measurement. Returns 1 if it was accepted, 0 if rejected as insane (in
 * which case NOTHING changes — the cadence holds). */
int    reac_dll_update(struct reac_dll *d, double measured_ppm);
/* The steered slot period. Never more than MAX_PPM away from nominal. */
long   reac_dll_period_ns(const struct reac_dll *d);
double reac_dll_applied_ppm(const struct reac_dll *d);

/* ---- the discipline: role + selection + DLL + an honest state ------------ *
 * UNLOCKED  no reference has ever been seen. Free-running on the host clock,
 *           applied == 0. We say so; we never call this locked.
 * LOCKING   a reference is selected and the loop is pulling toward it.
 * LOCKED    the residual has stayed inside LOCK_PPM for LOCK_UPDATES updates.
 * HOLDOVER  we had a reference and it went away. The last good rate is FROZEN and
 *           reported as holdover — we do not snap back to nominal, and we do not
 *           claim lock.
 */
enum reac_clock_state {
	REAC_CLOCK_UNLOCKED = 0,
	REAC_CLOCK_LOCKING,
	REAC_CLOCK_LOCKED,
	REAC_CLOCK_HOLDOVER,
};

const char *reac_clock_state_name(enum reac_clock_state s);

struct reac_clock_disc {
	enum reac_role role;     /* fixes WHICH hierarchy applies, for this process */
	struct reac_dll dll;
	enum reac_clock_source src;
	enum reac_clock_state  state;
	int      in_band;        /* consecutive updates inside LOCK_PPM */
	uint32_t generation;     /* bumped on every (src,state) change — the transcript
	                          * hook: the reference in use is never implicit */
	uint64_t holdovers;      /* times a reference vanished under us */
	uint64_t switches;       /* times the selected reference changed */
};

void reac_clock_disc_init(struct reac_clock_disc *c, enum reac_role role,
                          long nominal_period_ns);

/* One evaluation. `avail` is what is present RIGHT NOW; `measured_ppm` is the error
 * reported by the SELECTED reference and `have_measurement` says whether it is
 * fresh (a selected-but-silent reference still counts as present — that is one
 * evaluation with no new data, not a vanished reference). PURE: no clock read, no
 * I/O. */
void reac_clock_disc_update(struct reac_clock_disc *c, uint32_t avail,
                            double measured_ppm, int have_measurement);

/* The slot period to use. Exactly nominal while UNLOCKED. */
long reac_clock_disc_period_ns(const struct reac_clock_disc *c);

/* The one line the transcript must carry — role, pace and reference together, e.g.
 *   "master (pace: generated here) — locked to NIC/external PHC"
 *   "master (pace: generated here) — free-running (no reference)"
 *   "master (pace: generated here) — holdover: reference lost, holding the last
 *    good rate"
 *   "slave (pace: from the master) — locked to master cadence"
 * Writes at most `cap` bytes including the NUL and returns `out`. No allocation —
 * callable from a log drain. */
char *reac_clock_disc_describe(const struct reac_clock_disc *c, char *out, size_t cap);

/* ---- reference helpers (pure, so the live wiring stays a one-liner) ------ */

/* PipeWire publishes spa_io_clock.rate_diff: the driver clock's speed as a ratio of
 * CLOCK_MONOTONIC (>1.0 = the driver runs fast). That is our ppm measurement for
 * the GRAPH reference, already filtered by PipeWire's own DLL. */
double reac_clock_ppm_from_rate_diff(double rate_diff);

/* Is this graph driver clock an INDEPENDENT reference? spa_io_clock.name is
 * prefixed with its API; a `clock.system.*` driver (the dummy/freewheel timer
 * PipeWire falls back to with no hardware in the graph) is timed by CLOCK_MONOTONIC
 * itself, so following it is following our own free-run through a longer pipe —
 * which would let us report "locked" while nothing external disciplines anything.
 * Returns 0 for that, for an unnamed clock, and for NULL. */
int reac_clock_name_is_hardware(const char *name);

#endif /* REAC_CLOCK_H */
