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
 *                  locked hardware (reac_clock_name_is_hardware) that is also
 *                  GOOD ENOUGH to own a segment with (issue #77 — see the
 *                  QUALITY section below; the tier's rank in this list is
 *                  unchanged, what changed is who is admitted to it).
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

/* ---- reference QUALITY (issue #77) --------------------------------------- *
 * The hierarchy above ranks references by KIND. "Is it hardware?" — the only
 * question the GRAPH tier used to ask — is not the same question as "is it good
 * enough to OWN a REAC segment with". Owning the pace means every stagebox on the
 * segment locks to OUR rhythm; a rhythm taken from a display clock is jitter
 * propagated with authority. Better to know we are unqualified and say so than to
 * own a pace we cannot hold steadily.
 *
 * WHAT THIS DELIBERATELY IS NOT: a vendor allow-list. PLL quality is not
 * something PipeWire publishes, and a list of "good" device names is guaranteed
 * to demote every professional interface nobody has added to it yet — a failure
 * the operator would hit and we would never see. So the NAME heuristic has
 * exactly ONE power: to REJECT what is structurally disqualified. It can never
 * promote. Everything it does not recognise is UNGRADED — usable, no opinion —
 * and UNGRADED sits ABOVE MARGINAL on purpose, because MARGINAL is a demotion and
 * a demotion has to be EARNED by evidence, not handed out for being unfamiliar.
 * `reac_clock_name_quality`'s return type is the rule made unforgeable.
 *
 * The ladder, worst to best:
 *
 *   UNUSABLE    not a clock at all, or a clock whose audio timing is a
 *               by-product: `clock.system.*` / Dummy-Driver / Freewheel-Driver
 *               (software timers — already rejected, and kept rejected), and
 *               HDMI / DisplayPort sinks, whose word clock descends from a pixel
 *               clock picked to suit a monitor.
 *   MARGINAL    workable, flagged. Reached only by MEASUREMENT: a reference that
 *               proved jittery in practice, whatever its badge says. This is the
 *               issue's "ordinary consumer codec" case expressed in terms of
 *               something we can actually observe.
 *   UNGRADED    the honest default. No evidence either way. An unrecognised
 *               professional interface lands HERE, never below.
 *   GOOD        MEASURED stable over a long enough run. Earned, not claimed.
 *   DESIGNATED  the operator named this device as the reference. They know their
 *               hardware; this outranks every heuristic below it.
 *
 * THREE ASYMMETRIES, all deliberate:
 *
 *   - UNUSABLE IS STICKY. Neither designation nor a stable measurement lifts a
 *     display-derived clock: the defect is structural, not statistical, and a
 *     quiet ten minutes does not turn a pixel clock into a word clock. If the
 *     only candidate in the graph is an HDMI sink, the honest configuration is
 *     free-run — and saying so IS the feature.
 *   - MEASUREMENT CAN DEMOTE A DESIGNATED REFERENCE. Designation outranks the
 *     name heuristic; it does not outrank the evidence. An operator whose chosen
 *     reference is measurably wandering needs to be told, not agreed with.
 *   - MEASUREMENT NEVER PROMOTES THE BOX TIER. A stagebox slaved to US recovers
 *     its word clock from our own cadence, so its counter slope is our applied
 *     correction handed straight back and the residual is identically zero — the
 *     most perfectly "stable" series this module can ever see, and completely
 *     meaningless. We cannot distinguish that from a genuinely word-clocked box,
 *     so we refuse to award GOOD on it. Demotion still applies: a box that
 *     wanders really is wandering.
 */
enum reac_clock_quality {
	REAC_CLOCK_Q_UNUSABLE = 0,   /* must never own a segment                    */
	REAC_CLOCK_Q_MARGINAL,       /* workable, flagged — an EARNED demotion      */
	REAC_CLOCK_Q_UNGRADED,       /* no evidence either way — the honest default */
	REAC_CLOCK_Q_GOOD,           /* measured stable over a long enough run      */
	REAC_CLOCK_Q_DESIGNATED,     /* the operator named it                       */
};
#define REAC_CLOCK_Q_COUNT 5

/* "unusable" / "marginal" / "ungraded" / "good" / "operator-designated". */
const char *reac_clock_quality_name(enum reac_clock_quality q);

/* ELIGIBILITY (issue #77 item 5). Is this tier fit to own a REAC segment's pace?
 * Reporting-only and PURE: this module never yanks a reference out from under a
 * running segment on its own opinion — that decision belongs to whoever offers
 * the master/follower control (the Setup -> Clock panel, openmixer). What we owe
 * them is an unambiguous answer, and MARGINAL answering "no" is the whole point:
 * free-running while owning a segment is a legitimate emergency configuration,
 * not something to slide into by accident. */
int reac_clock_quality_can_own(enum reac_clock_quality q);

/* The NAME heuristic, and the WHOLE of it. Returns UNUSABLE for a name that is
 * structurally disqualified (software timer, display-derived sink) and UNGRADED
 * for absolutely everything else — including names we have never seen. It has no
 * way to express "good", by construction. Case-insensitive; NULL/empty is
 * UNUSABLE (an unnamed clock was never admissible). */
enum reac_clock_quality reac_clock_name_quality(const char *name);

/* Does `name` match the operator's designation? Case-insensitive SUBSTRING: the
 * operator types "Babyface", not `alsa_output.usb-RME_Babyface_Pro-00.pro-output-0`.
 * An empty or NULL designation matches nothing (designation is opt-in, never a
 * default that quietly promotes the first device to appear). */
int reac_clock_name_is_designated(const char *name, const char *designation);

/* The publisher's whole INFERRED verdict on a device, in one call: the name
 * heuristic, with the operator's designation on top of it. Designation promotes
 * an UNGRADED device to DESIGNATED; it does NOT rescue an UNUSABLE one, because
 * designation is the operator choosing among plausible references, not a way to
 * declare a display clock into a word clock. Measurement is folded in later and
 * elsewhere (reac_clock_quality_apply) — this call is name-only, which is why it
 * can be evaluated on the RT graph thread. */
enum reac_clock_quality reac_clock_grade_name(const char *name,
                                              const char *designation);

/* reac_clock_select with an ADMISSION BAR. `q` is the per-source quality indexed
 * by enum reac_clock_source (NULL = every source UNGRADED, i.e. exactly
 * reac_clock_select); a candidate whose quality is below `min_q` is skipped as
 * though it were absent, and the search continues DOWN the same hierarchy — the
 * ordering of the tiers is not touched by any of this, only who is let into one.
 *
 * The default bar is MARGINAL (see reac_clock_disc_init), which excludes exactly
 * the structurally disqualified and nothing else. It is deliberately NOT set to
 * UNGRADED: a measured demotion would then eject a reference mid-run, the
 * stability accumulator would reset with the source change, the reference would
 * be re-admitted, measure badly again — a flap loop, on a live segment, driven by
 * our own opinion. We flag instead, loudly, and leave the ejection to the
 * operator's control. */
enum reac_clock_source reac_clock_select_graded(enum reac_role role, uint32_t avail,
                                                const enum reac_clock_quality *q,
                                                enum reac_clock_quality min_q);

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

/* ---- the MEASURED stability signal (issue #77) ---------------------------- *
 * The loop already computes a residual every update. Its MEAN tells us whether we
 * are tracking (that is the LOCK test above). Its VARIANCE tells us something the
 * device's name never can: how steady the reference actually is. This is the one
 * quality input that is measured rather than inferred, and it is allowed to
 * overrule everything inferred — see reac_clock_quality_apply.
 *
 * WHAT IS AND IS NOT COUNTED. Samples are folded in only while the loop is
 * already IN BAND (resid_ema < LOCK_PPM). During acquisition the residual is a
 * decaying ramp — a slew-limited glide toward a step is a perfectly healthy loop
 * doing its job, and its variance is enormous. Counting it would demote every
 * good reference for the first few seconds after it appears. So acquisition
 * contributes nothing, and a reference that never gets in band earns no verdict
 * at all; that it is stuck at LOCKING is already the report an operator needs.
 *
 * The accumulator resets on a SOURCE CHANGE (the residual series belongs to the
 * old reference) and NEVER on holdover — `applied_ppm` is the holdover value and
 * the header's holdover contract stays exactly as it was.
 *
 * TWO THRESHOLDS, ONE VERDICT, WITH HYSTERESIS. Between them the verdict does not
 * move; only crossing a threshold changes it, so a reference sitting near a
 * boundary cannot flap the reported tier.
 *
 * THE BAND IS WIDE, AND ITS WIDTH IS MEASURED, NOT TASTE. reac_rx recomputes its
 * counter slope ~4x/s and carries a couple of ppm of window noise: a +-3 ppm
 * uniform series (sigma 1.73) is what a PERFECT reference looks like through our
 * own instrument, and a 50-sample EWMA of its variance excursions to ~2.2 ppm.
 * The demotion threshold therefore sits at 4 ppm — clear of the instrument's own
 * noise floor with room to spare — so that "unstable" always means the reference,
 * never the ruler. Between the two thresholds the honest verdict is "we cannot
 * tell", and that is a verdict this module is willing to publish. A wrong one is
 * not. (The gap is pinned by a replay in tests/test_reac_clock.c; narrowing it
 * fails there before it can demote an operator's converter on the rig.) */
#define REAC_DLL_STAB_ALPHA     0.02   /* EWMA over ~50 updates (~6 s at 8 Hz)  */
#define REAC_DLL_STAB_MIN_N       32   /* in-band samples before ANY verdict    */
#define REAC_DLL_STABLE_PPM      0.5   /* sigma under this -> measured STABLE   */
#define REAC_DLL_UNSTABLE_PPM    4.0   /* sigma over this  -> measured UNSTABLE */

enum reac_clock_stability {
	REAC_CLOCK_STAB_UNKNOWN = 0,  /* not enough in-band evidence, or in the band
	                               * between the thresholds and never yet outside */
	REAC_CLOCK_STAB_STABLE,
	REAC_CLOCK_STAB_UNSTABLE,
};

const char *reac_clock_stability_name(enum reac_clock_stability s);

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
	/* The stability accumulator (#77). `resid_mean` is the EWMA centre the
	 * variance is taken about — not assumed to be zero, so a reference with a
	 * small standing bias is judged on its WANDER, not on its offset. */
	double resid_mean;
	double resid_var;       /* EWMA of (residual - mean)^2, in ppm^2           */
	uint64_t stab_n;        /* in-band samples since the last stability reset  */
	uint8_t stab_verdict;   /* enum reac_clock_stability, hysteretic           */
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

/* The measured wander of the reference, in ppm (sqrt of the EWMA variance).
 * Meaningful only once reac_dll_stability() has left UNKNOWN; before that it is
 * simply what has been seen so far. */
double reac_dll_resid_sigma(const struct reac_dll *d);
/* The hysteretic verdict. UNKNOWN until there is enough in-band evidence. */
enum reac_clock_stability reac_dll_stability(const struct reac_dll *d);
/* Forget the stability series (a different reference is being measured now).
 * Does NOT touch applied_ppm, updates, or the lock EMA — holdover and the lock
 * contract are unchanged by anything in #77. */
void reac_dll_stability_reset(struct reac_dll *d);

/* Fold the measured verdict into the inferred grade for a given tier. PURE and
 * table-like on purpose, so every one of the asymmetries documented in the
 * QUALITY section above is one line you can point at and one test case:
 *
 *   inferred UNUSABLE       -> UNUSABLE   (structural; nothing lifts it)
 *   measured UNSTABLE       -> MARGINAL   (beats the name AND the designation)
 *   measured STABLE, BOX    -> unchanged  (the closed loop fakes perfection)
 *   measured STABLE, other  -> at least GOOD
 *   otherwise               -> unchanged
 */
enum reac_clock_quality reac_clock_quality_apply(enum reac_clock_source src,
                                                 enum reac_clock_quality inferred,
                                                 enum reac_clock_stability measured);

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
	uint32_t generation;     /* bumped on every (src,state,quality) change — the
	                          * transcript hook: neither the reference in use nor
	                          * how good we judge it is ever implicit */
	uint64_t holdovers;      /* times a reference vanished under us */
	uint64_t switches;       /* times the selected reference changed */

	/* Quality (#77). `quality[]` is what the PUBLISHER inferred about each source
	 * from its device name and the operator's designation — the discipline does
	 * not parse names, it only ranks. `src_quality` is the EFFECTIVE tier of the
	 * currently selected reference: the inferred grade with the measured verdict
	 * folded in (reac_clock_quality_apply). Both default to UNGRADED, so a caller
	 * that never grades anything gets today's behaviour to the bit. */
	enum reac_clock_quality quality[REAC_CLOCK_SRC_COUNT];
	enum reac_clock_quality min_quality;   /* admission bar; default MARGINAL */
	enum reac_clock_quality src_quality;   /* effective tier of `src` */
};

void reac_clock_disc_init(struct reac_clock_disc *c, enum reac_role role,
                          long nominal_period_ns);

/* Tell the discipline what the publisher inferred about one source. Cheap and
 * idempotent; call it whenever the device behind a reference changes (or every
 * evaluation — it is a store). Out-of-range sources are ignored. */
void reac_clock_disc_set_quality(struct reac_clock_disc *c,
                                 enum reac_clock_source src,
                                 enum reac_clock_quality q);

/* The EFFECTIVE quality of the reference currently in use — inferred grade plus
 * measured stability. UNGRADED while free-running: there is no reference to have
 * an opinion about. */
enum reac_clock_quality reac_clock_disc_quality(const struct reac_clock_disc *c);

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

/* The same line from the three fields alone, for a consumer that carries them
 * across a thread boundary (the pacer's RT event ring) rather than holding the
 * discipline itself. */
char *reac_clock_describe(enum reac_role role, enum reac_clock_source src,
                          enum reac_clock_state state, char *out, size_t cap);

/* The COMPLETE line: the three fields above plus the DEVICE behind the reference
 * and how good we judge it, e.g.
 *   "master (pace: generated here) — locked to graph clock (RME Babyface Pro),
 *    quality: operator-designated"
 * `label` may be NULL/empty (no device known) and an UNGRADED quality prints no
 * clause at all — we do not editorialise where we have no opinion, and an extra
 * word on every line is how a transcript stops being read. Neither is printed
 * for a state with no reference (free-run, holdover): naming a device we are no
 * longer following would be the same lie the state names exist to prevent.
 * reac_clock_describe is exactly this with no label and no opinion. */
char *reac_clock_describe_full(enum reac_role role, enum reac_clock_source src,
                               enum reac_clock_state state,
                               enum reac_clock_quality q, const char *label,
                               char *out, size_t cap);

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
 * Returns 0 for that, for an unnamed clock, and for NULL.
 *
 * KEPT SEPARATE from reac_clock_name_quality on purpose. This one answers "is
 * there an independent oscillator behind this at all" — an ADMISSION question,
 * and the only one the publisher asked before #77. The other answers "and is that
 * oscillator fit to own a segment". A display sink passes this test and fails
 * that one, which is precisely the gap #77 exists to close; collapsing the two
 * would hide which of the two reasons a candidate was refused for. */
int reac_clock_name_is_hardware(const char *name);

#endif /* REAC_CLOCK_H */
