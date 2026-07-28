// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_clock.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the two hierarchies ------------------------------------------------- *
 * Written as data, not as branches, so the ordering IS the definition and a test
 * can assert it directly. Each source appears once per list, so "best available"
 * is a total order with no ties to resolve. */

static const enum reac_clock_source master_hier[] = {
	REAC_CLOCK_SRC_PHC,     /* independent of both the host and the segment  */
	REAC_CLOCK_SRC_GRAPH,   /* hardware-driven graph only (see the header)   */
	REAC_CLOCK_SRC_BOX,     /* real only when the box is itself clock master */
};

/* Descriptive, not prescriptive: reac_slave is arrival-driven and runs no pacer,
 * so this row is what its reporting NAMES, not something a DLL steers. */
static const enum reac_clock_source slave_hier[] = {
	REAC_CLOCK_SRC_WIRE,    /* the master's cadence. Nothing outranks it.    */
};

const enum reac_clock_source *reac_clock_hierarchy(enum reac_role role, int *n)
{
	if (role == REAC_ROLE_SLAVE) {
		if (n) *n = (int)(sizeof slave_hier / sizeof slave_hier[0]);
		return slave_hier;
	}
	if (n) *n = (int)(sizeof master_hier / sizeof master_hier[0]);
	return master_hier;
}

enum reac_clock_source reac_clock_select_graded(enum reac_role role, uint32_t avail,
                                                const enum reac_clock_quality *q,
                                                enum reac_clock_quality min_q)
{
	int n = 0;
	const enum reac_clock_source *h = reac_clock_hierarchy(role, &n);
	for (int i = 0; i < n; i++) {
		if (!(avail & (1u << h[i])))
			continue;
		/* The bar is applied INSIDE the walk, not before it: a candidate that
		 * fails it is skipped and the search carries on down the SAME list, so a
		 * disqualified graph clock falls through to the box tier exactly as an
		 * absent one would. The tier ordering is untouched by quality — #77
		 * refines who is admitted to a tier, never where the tier sits. */
		if (q && q[h[i]] < min_q)
			continue;
		return h[i];
	}
	/* Anything available but not in THIS role's hierarchy is deliberately not
	 * considered: a slave with a NIC PHC and no master on the wire free-runs and
	 * says so — it does not quietly discipline its upstream to a local clock. */
	return REAC_CLOCK_SRC_FREERUN;
}

enum reac_clock_source reac_clock_select(enum reac_role role, uint32_t avail)
{
	return reac_clock_select_graded(role, avail, NULL, REAC_CLOCK_Q_UNUSABLE);
}

const char *reac_clock_source_name(enum reac_clock_source s)
{
	switch (s) {
	case REAC_CLOCK_SRC_WIRE:    return "master cadence";
	case REAC_CLOCK_SRC_PHC:     return "NIC/external PHC";
	case REAC_CLOCK_SRC_GRAPH:   return "graph clock";
	case REAC_CLOCK_SRC_BOX:     return "box counter slope";
	case REAC_CLOCK_SRC_FREERUN: return "host monotonic clock";
	}
	return "?";
}

const char *reac_clock_quality_name(enum reac_clock_quality q)
{
	switch (q) {
	case REAC_CLOCK_Q_UNUSABLE:   return "unusable";
	case REAC_CLOCK_Q_MARGINAL:   return "marginal";
	case REAC_CLOCK_Q_UNGRADED:   return "ungraded";
	case REAC_CLOCK_Q_GOOD:       return "good";
	case REAC_CLOCK_Q_DESIGNATED: return "operator-designated";
	}
	return "?";
}

const char *reac_clock_stability_name(enum reac_clock_stability s)
{
	switch (s) {
	case REAC_CLOCK_STAB_UNKNOWN:  return "unknown";
	case REAC_CLOCK_STAB_STABLE:   return "stable";
	case REAC_CLOCK_STAB_UNSTABLE: return "unstable";
	}
	return "?";
}

int reac_clock_quality_can_own(enum reac_clock_quality q)
{
	/* UNGRADED qualifies. "We have no evidence against this device" is not a
	 * reason to refuse an operator their rig — refusing on ignorance is exactly
	 * the vendor-allow-list failure wearing a different hat. MARGINAL does not:
	 * that verdict was measured, and it is the one #77 wants surfaced. */
	return q >= REAC_CLOCK_Q_UNGRADED;
}

const char *reac_clock_state_name(enum reac_clock_state s)
{
	switch (s) {
	case REAC_CLOCK_UNLOCKED: return "unlocked";
	case REAC_CLOCK_LOCKING:  return "locking";
	case REAC_CLOCK_LOCKED:   return "locked";
	case REAC_CLOCK_HOLDOVER: return "holdover";
	}
	return "?";
}

/* ---- the DLL ------------------------------------------------------------- */

void reac_dll_init(struct reac_dll *d, long nominal_period_ns)
{
	memset(d, 0, sizeof *d);
	d->nominal_ns = nominal_period_ns;
}

int reac_dll_update(struct reac_dll *d, double measured_ppm)
{
	/* SANE gate first: a garbage sample must not move the loop even by a slew
	 * step. Reject and hold. */
	if (!(measured_ppm > -REAC_DLL_SANE_PPM && measured_ppm < REAC_DLL_SANE_PPM)) {
		d->rejected++;
		return 0;
	}

	double residual = measured_ppm - d->applied_ppm;
	double target = d->applied_ppm + REAC_DLL_KP * residual;

	if (target > REAC_DLL_MAX_PPM) {
		target = REAC_DLL_MAX_PPM;
		d->clamped++;
	} else if (target < -REAC_DLL_MAX_PPM) {
		target = -REAC_DLL_MAX_PPM;
		d->clamped++;
	}

	/* SLEW: follow even a step in the measurement as a glide. */
	double step = target - d->applied_ppm;
	if (step > REAC_DLL_SLEW_PPM)
		step = REAC_DLL_SLEW_PPM;
	else if (step < -REAC_DLL_SLEW_PPM)
		step = -REAC_DLL_SLEW_PPM;

	d->applied_ppm += step;
	d->last_residual = residual;

	/* Was the loop TRACKING when this sample arrived? Asked BEFORE the sample is
	 * folded into the EMA, and that ordering is the whole point — see below. */
	int tracking = d->updates > 0 && d->resid_ema < REAC_DLL_LOCK_PPM;

	double mag = residual < 0.0 ? -residual : residual;
	if (d->updates == 0)
		d->resid_ema = mag;      /* seed: never in-band before we have measured */
	else
		d->resid_ema += REAC_DLL_RESID_ALPHA * (mag - d->resid_ema);

	d->updates++;

	/* ---- the measured stability signal (#77) -------------------------- *
	 * IN BAND ONLY. Acquisition is a decaying (and, after a step, slew-limited)
	 * ramp: healthy loop behaviour with a huge variance. Folding it in would
	 * demote every good reference for the first seconds of its life.
	 *
	 * But the gate is on the state BEFORE this sample, so an OUTLIER that lands
	 * while we were tracking is COUNTED — it is precisely the evidence we came
	 * for — while the recovery ramp behind it, during which the EMA is elevated,
	 * is not. Gating on the state after would silently discard every spike and
	 * leave a jumpy reference looking immaculate. */
	if (!tracking)
		return 1;

	if (d->stab_n == 0) {
		d->resid_mean = residual;   /* seed the centre; no deviation yet */
		d->resid_var = 0.0;
	} else {
		/* The standard EWMA mean/variance recurrence: deviate about the mean
		 * BEFORE it absorbs this sample, so a step contributes to the variance
		 * once rather than being partly hidden inside the updated centre. */
		double delta = residual - d->resid_mean;
		d->resid_mean += REAC_DLL_STAB_ALPHA * delta;
		d->resid_var += REAC_DLL_STAB_ALPHA * (delta * delta - d->resid_var);
	}
	d->stab_n++;

	if (d->stab_n >= REAC_DLL_STAB_MIN_N) {
		double sigma = sqrt(d->resid_var);
		/* Hysteresis: only CROSSING a threshold moves the verdict. Between them
		 * the last verdict stands, so a reference parked near a boundary cannot
		 * flap the tier we report (and, through it, the transcript). */
		if (sigma < REAC_DLL_STABLE_PPM)
			d->stab_verdict = (uint8_t)REAC_CLOCK_STAB_STABLE;
		else if (sigma > REAC_DLL_UNSTABLE_PPM)
			d->stab_verdict = (uint8_t)REAC_CLOCK_STAB_UNSTABLE;
	}
	return 1;
}

double reac_dll_resid_sigma(const struct reac_dll *d)
{
	return sqrt(d->resid_var);
}

enum reac_clock_stability reac_dll_stability(const struct reac_dll *d)
{
	if (d->stab_n < REAC_DLL_STAB_MIN_N)
		return REAC_CLOCK_STAB_UNKNOWN;
	return (enum reac_clock_stability)d->stab_verdict;
}

void reac_dll_stability_reset(struct reac_dll *d)
{
	d->resid_mean = 0.0;
	d->resid_var = 0.0;
	d->stab_n = 0;
	d->stab_verdict = (uint8_t)REAC_CLOCK_STAB_UNKNOWN;
	/* applied_ppm, updates, resid_ema and the counters are UNTOUCHED: the loop's
	 * memory is the holdover value and the lock claim is a separate contract. */
}

double reac_dll_applied_ppm(const struct reac_dll *d)
{
	return d->applied_ppm;
}

long reac_dll_period_ns(const struct reac_dll *d)
{
	/* applied is already bounded, so this can never leave the band; dividing (not
	 * multiplying by 1-ppm) keeps rate and period exactly reciprocal. */
	double p = (double)d->nominal_ns / (1.0 + d->applied_ppm / 1e6);
	long ns = (long)(p + 0.5);
	return ns > 0 ? ns : d->nominal_ns;
}

enum reac_clock_quality reac_clock_quality_apply(enum reac_clock_source src,
                                                 enum reac_clock_quality inferred,
                                                 enum reac_clock_stability measured)
{
	/* Structural disqualification is sticky: no measurement, and no operator,
	 * turns a pixel clock into a word clock. */
	if (inferred == REAC_CLOCK_Q_UNUSABLE)
		return REAC_CLOCK_Q_UNUSABLE;
	/* MEASURED BEATS INFERRED — including a designation. The operator picked the
	 * device; the device is the one wandering. */
	if (measured == REAC_CLOCK_STAB_UNSTABLE)
		return REAC_CLOCK_Q_MARGINAL;
	if (measured == REAC_CLOCK_STAB_STABLE) {
		/* A box slaved to US hands our own correction back, so its residual is
		 * identically zero — flawless and meaningless. Never promote on it. */
		if (src == REAC_CLOCK_SRC_BOX)
			return inferred;
		if (inferred < REAC_CLOCK_Q_GOOD)
			return REAC_CLOCK_Q_GOOD;
	}
	return inferred;
}

/* ---- the discipline ------------------------------------------------------ */

void reac_clock_disc_init(struct reac_clock_disc *c, enum reac_role role,
                          long nominal_period_ns)
{
	memset(c, 0, sizeof *c);
	c->role = role;
	reac_dll_init(&c->dll, nominal_period_ns);
	c->src = REAC_CLOCK_SRC_FREERUN;
	c->state = REAC_CLOCK_UNLOCKED;
	/* UNGRADED, not the memset's zero (which is UNUSABLE): a caller that never
	 * calls set_quality must get exactly the pre-#77 behaviour, and "nobody has
	 * graded this" has to mean "no opinion", never "rejected". */
	for (int s = 0; s < REAC_CLOCK_SRC_COUNT; s++)
		c->quality[s] = REAC_CLOCK_Q_UNGRADED;
	c->min_quality = REAC_CLOCK_Q_MARGINAL;   /* excludes UNUSABLE, nothing else */
	c->src_quality = REAC_CLOCK_Q_UNGRADED;
}

void reac_clock_disc_set_quality(struct reac_clock_disc *c,
                                 enum reac_clock_source src,
                                 enum reac_clock_quality q)
{
	if ((unsigned)src < REAC_CLOCK_SRC_COUNT)
		c->quality[src] = q;
}

enum reac_clock_quality reac_clock_disc_quality(const struct reac_clock_disc *c)
{
	return c->src_quality;
}

void reac_clock_disc_update(struct reac_clock_disc *c, uint32_t avail,
                            double measured_ppm, int have_measurement)
{
	enum reac_clock_source prev_src = c->src;
	enum reac_clock_state prev_state = c->state;
	enum reac_clock_quality prev_quality = c->src_quality;
	enum reac_clock_source src = reac_clock_select_graded(c->role, avail, c->quality,
	                                                      c->min_quality);

	if (src != prev_src) {
		c->switches++;
		c->in_band = 0;
		/* The residual series we have been accumulating belongs to the OLD
		 * reference; carrying it over would let one device's steadiness vouch for
		 * another's. applied_ppm survives — that is holdover, a separate promise. */
		reac_dll_stability_reset(&c->dll);
	}
	c->src = src;

	if (src == REAC_CLOCK_SRC_FREERUN) {
		/* Honest degradation. Never touch the DLL here: if it holds a correction
		 * that IS the last good rate, and we keep emitting at it (holdover)
		 * rather than snapping the cadence back to nominal. If it never had one,
		 * we are plainly free-running and we say so. */
		if (c->dll.updates > 0) {
			if (prev_state != REAC_CLOCK_HOLDOVER)
				c->holdovers++;
			c->state = REAC_CLOCK_HOLDOVER;
		} else {
			c->state = REAC_CLOCK_UNLOCKED;
		}
	} else if (have_measurement && reac_dll_update(&c->dll, measured_ppm)) {
		if (c->dll.resid_ema < REAC_DLL_LOCK_PPM)
			c->in_band++;
		else
			c->in_band = 0;
		c->state = (c->in_band >= REAC_DLL_LOCK_UPDATES) ? REAC_CLOCK_LOCKED
		                                                 : REAC_CLOCK_LOCKING;
	} else if (c->state == REAC_CLOCK_UNLOCKED || c->state == REAC_CLOCK_HOLDOVER) {
		/* A reference is present but has not produced a usable sample yet (no
		 * fresh data, or it was rejected as insane). We are pulling toward it,
		 * not locked to it — and a rejected sample must never promote us. */
		c->state = REAC_CLOCK_LOCKING;
	}

	/* The effective tier of what we are actually following (#77): what the
	 * publisher inferred, with the measured verdict folded in. Free-run gets
	 * UNGRADED rather than UNUSABLE — there is no reference to have an opinion
	 * about, and the state already says so in words. */
	c->src_quality = (src == REAC_CLOCK_SRC_FREERUN)
		? REAC_CLOCK_Q_UNGRADED
		: reac_clock_quality_apply(src, c->quality[src], reac_dll_stability(&c->dll));

	/* Quality joins source and state as a reportable change: a reference that
	 * silently degrades from good to marginal under a running segment is exactly
	 * the event an operator must not have to go looking for. */
	if (c->src != prev_src || c->state != prev_state ||
	    c->src_quality != prev_quality)
		c->generation++;
}

long reac_clock_disc_period_ns(const struct reac_clock_disc *c)
{
	if (c->state == REAC_CLOCK_UNLOCKED)
		return c->dll.nominal_ns;
	return reac_dll_period_ns(&c->dll);
}

char *reac_clock_disc_describe(const struct reac_clock_disc *c, char *out, size_t cap)
{
	return reac_clock_describe(c->role, c->src, c->state, out, cap);
}

char *reac_clock_describe(enum reac_role r, enum reac_clock_source src,
                          enum reac_clock_state state, char *out, size_t cap)
{
	return reac_clock_describe_full(r, src, state, REAC_CLOCK_Q_UNGRADED, NULL,
	                                out, cap);
}

char *reac_clock_describe_full(enum reac_role r, enum reac_clock_source src,
                               enum reac_clock_state state,
                               enum reac_clock_quality q, const char *label,
                               char *out, size_t cap)
{
	/* Role AND pace AND reference, always together. A reference name on its own
	 * would leave the reader guessing whether we generate the cadence or follow
	 * it — the exact ambiguity this module exists to remove. */
	const char *role = reac_role_name(r);
	const char *pace = r == REAC_ROLE_SLAVE ? "from the master" : "generated here";
	/* The device, and the opinion, are suffixes on the reference clause — so the
	 * clause itself is byte-identical to what the transcript printed before #77
	 * and every existing reader keeps matching. Both are omitted when there is
	 * nothing to say: no device known, or no opinion worth a word. */
	char dev[96];
	dev[0] = '\0';
	if (label && label[0])
		snprintf(dev, sizeof dev, " (%s)", label);
	char qual[64];
	qual[0] = '\0';
	if (q != REAC_CLOCK_Q_UNGRADED)
		snprintf(qual, sizeof qual, ", quality: %s", reac_clock_quality_name(q));

	switch (state) {
	case REAC_CLOCK_LOCKED:
		snprintf(out, cap, "%s (pace: %s) — locked to %s%s%s", role, pace,
		         reac_clock_source_name(src), dev, qual);
		break;
	case REAC_CLOCK_LOCKING:
		snprintf(out, cap, "%s (pace: %s) — acquiring %s%s%s", role, pace,
		         reac_clock_source_name(src), dev, qual);
		break;
	case REAC_CLOCK_HOLDOVER:
		snprintf(out, cap, "%s (pace: %s) — holdover: reference lost, holding "
		         "the last good rate", role, pace);
		break;
	case REAC_CLOCK_UNLOCKED:
	default:
		snprintf(out, cap, "%s (pace: %s) — free-running (no reference)",
		         role, pace);
		break;
	}
	return out;
}

/* ---- reference helpers --------------------------------------------------- */

double reac_clock_ppm_from_rate_diff(double rate_diff)
{
	/* A driver that has not measured itself yet publishes 0.0, which as a RATIO
	 * reads as "the clock has stopped" and as a ppm as -1e6. Treat it as "no
	 * information": zero error. The SANE gate would reject -1e6 anyway; this keeps
	 * the rejected counter meaning "the reference is wrong", not "the reference
	 * has not started". */
	if (rate_diff <= 0.0)
		return 0.0;
	return (rate_diff - 1.0) * 1e6;
}

int reac_clock_name_is_hardware(const char *name)
{
	if (!name || name[0] == '\0')
		return 0;
	if (strncmp(name, "clock.system.", 13) == 0)
		return 0;
	return 1;
}

/* ---- the name heuristic (#77) -------------------------------------------- *
 * ASCII case folding and a hand-rolled substring search rather than strcasestr:
 * that one is a GNU extension, and this module is deliberately dependency-free so
 * it compiles into every offline test target unchanged. Device names are ASCII
 * PipeWire identifiers, so a locale-independent fold is the correct one anyway. */

static char ci_fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_contains(const char *hay, const char *needle)
{
	if (!hay || !needle || !needle[0])
		return 0;
	for (size_t i = 0; hay[i]; i++) {
		size_t j = 0;
		while (needle[j] && ci_fold(hay[i + j]) == ci_fold(needle[j]))
			j++;
		if (!needle[j])
			return 1;
	}
	return 0;
}

/* Structurally disqualified names. This list is the ONLY thing the heuristic
 * knows, and every entry is here because of WHAT THE CLOCK IS, never because of
 * who made it:
 *
 *   dummy / freewheel / clock.system.*  — software timers driven by
 *       CLOCK_MONOTONIC. Following one is following our own free-run through a
 *       longer pipe while reporting "locked". Not a clock.
 *   hdmi / displayport / display-port   — the audio clock is a by-product of a
 *       pixel clock chosen to suit a monitor's mode, and it changes when the mode
 *       does. Perfectly fine for a TV; catastrophic as the thing an entire REAC
 *       segment is told to lock to.
 *
 * Nothing may be added here that amounts to "we have not heard of this vendor". */
static const char *const unusable_names[] = {
	"dummy", "freewheel", "hdmi", "displayport", "display-port",
};

enum reac_clock_quality reac_clock_name_quality(const char *name)
{
	if (!name || name[0] == '\0')
		return REAC_CLOCK_Q_UNUSABLE;   /* an unnamed clock was never admissible */
	if (!reac_clock_name_is_hardware(name))
		return REAC_CLOCK_Q_UNUSABLE;   /* clock.system.* — the software timer */
	for (size_t i = 0; i < sizeof unusable_names / sizeof unusable_names[0]; i++)
		if (ci_contains(name, unusable_names[i]))
			return REAC_CLOCK_Q_UNUSABLE;
	/* Everything else. NOT "we recognise this and approve" — "we have no reason
	 * to doubt it", which is a different and much more honest claim, and the one
	 * that keeps an unfamiliar professional interface out of the bin. */
	return REAC_CLOCK_Q_UNGRADED;
}

int reac_clock_name_is_designated(const char *name, const char *designation)
{
	if (!designation || designation[0] == '\0')
		return 0;
	return ci_contains(name, designation);
}

enum reac_clock_quality reac_clock_grade_name(const char *name,
                                              const char *designation)
{
	enum reac_clock_quality q = reac_clock_name_quality(name);
	if (q == REAC_CLOCK_Q_UNUSABLE)
		return q;   /* sticky: an operator cannot designate a pixel clock good */
	if (reac_clock_name_is_designated(name, designation))
		return REAC_CLOCK_Q_DESIGNATED;
	return q;
}
