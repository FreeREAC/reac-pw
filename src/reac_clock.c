// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_clock.h"

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

enum reac_clock_source reac_clock_select(enum reac_role role, uint32_t avail)
{
	int n = 0;
	const enum reac_clock_source *h = reac_clock_hierarchy(role, &n);
	for (int i = 0; i < n; i++)
		if (avail & (1u << h[i]))
			return h[i];
	/* Anything available but not in THIS role's hierarchy is deliberately not
	 * considered: a slave with a NIC PHC and no master on the wire free-runs and
	 * says so — it does not quietly discipline its upstream to a local clock. */
	return REAC_CLOCK_SRC_FREERUN;
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

	double mag = residual < 0.0 ? -residual : residual;
	if (d->updates == 0)
		d->resid_ema = mag;      /* seed: never in-band before we have measured */
	else
		d->resid_ema += REAC_DLL_RESID_ALPHA * (mag - d->resid_ema);

	d->updates++;
	return 1;
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

/* ---- the discipline ------------------------------------------------------ */

void reac_clock_disc_init(struct reac_clock_disc *c, enum reac_role role,
                          long nominal_period_ns)
{
	memset(c, 0, sizeof *c);
	c->role = role;
	reac_dll_init(&c->dll, nominal_period_ns);
	c->src = REAC_CLOCK_SRC_FREERUN;
	c->state = REAC_CLOCK_UNLOCKED;
}

void reac_clock_disc_update(struct reac_clock_disc *c, uint32_t avail,
                            double measured_ppm, int have_measurement)
{
	enum reac_clock_source prev_src = c->src;
	enum reac_clock_state prev_state = c->state;
	enum reac_clock_source src = reac_clock_select(c->role, avail);

	if (src != prev_src) {
		c->switches++;
		c->in_band = 0;
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

	if (c->src != prev_src || c->state != prev_state)
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
	/* Role AND pace AND reference, always together. A reference name on its own
	 * would leave the reader guessing whether we generate the cadence or follow
	 * it — the exact ambiguity this module exists to remove. */
	const char *role = reac_role_name(c->role);
	const char *pace = c->role == REAC_ROLE_SLAVE ? "from the master"
	                                              : "generated here";
	switch (c->state) {
	case REAC_CLOCK_LOCKED:
		snprintf(out, cap, "%s (pace: %s) — locked to %s", role, pace,
		         reac_clock_source_name(c->src));
		break;
	case REAC_CLOCK_LOCKING:
		snprintf(out, cap, "%s (pace: %s) — acquiring %s", role, pace,
		         reac_clock_source_name(c->src));
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
