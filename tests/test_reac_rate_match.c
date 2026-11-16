// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The follower rate the source node hands PipeWire's resampler must stay inside
 * the bound the sink uses (REAC_RATE_MATCH_MAX_PPM), whatever pace the master
 * measures at, and every clamp is counted. libreac publishes ppm * 1000 as an int
 * from the counter slope, so a master at 0.5x or 2x arrives as -5e8 / +1e9 and
 * used to reach the resampler as a rate of 0.5 / 2.0. A non-finite value (the
 * helper takes a double) must be refused to nominal and counted. */
#include "reac_rate_match.h"

#include <math.h>
#include <stdio.h>

static int fails;
#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static double bound(void) { return REAC_RATE_MATCH_MAX_PPM / 1e6; }

int main(void)
{
	_Atomic unsigned long long clamped = 0;

	/* In-bound values pass through untouched and are not counted. */
	double r = reac_rate_match_from_ppm_milli(1500.0 * 1000.0, &clamped);
	CHK(fabs(r - 1.0015) < 1e-12);
	r = reac_rate_match_from_ppm_milli(-5000.0 * 1000.0, &clamped);
	CHK(fabs(r - (1.0 - bound())) < 1e-12);
	CHK(atomic_load(&clamped) == 0);

	/* A master at 0.5x: counter slope half nominal = -500000 ppm. */
	r = reac_rate_match_from_ppm_milli(-500000.0 * 1000.0, &clamped);
	fprintf(stderr, "master 0.5x -> rate %.9f\n", r);
	CHK(fabs(r - (1.0 - bound())) < 1e-12);
	CHK(atomic_load(&clamped) == 1);

	/* A master at 2x: +1000000 ppm (the int the feeder can still represent). */
	r = reac_rate_match_from_ppm_milli(1000000.0 * 1000.0, &clamped);
	fprintf(stderr, "master 2x -> rate %.9f\n", r);
	CHK(fabs(r - (1.0 + bound())) < 1e-12);
	CHK(atomic_load(&clamped) == 2);

	/* Extremes of the published int. */
	r = reac_rate_match_from_ppm_milli(2147483647.0, &clamped);
	CHK(fabs(r - (1.0 + bound())) < 1e-12);
	r = reac_rate_match_from_ppm_milli(-2147483648.0, &clamped);
	CHK(fabs(r - (1.0 - bound())) < 1e-12);
	CHK(atomic_load(&clamped) == 4);

	/* Non-finite: refused to nominal, counted. */
	r = reac_rate_match_from_ppm_milli(NAN, &clamped);
	CHK(r == 1.0);
	r = reac_rate_match_from_ppm_milli(INFINITY, &clamped);
	CHK(r == 1.0);
	r = reac_rate_match_from_ppm_milli(-INFINITY, &clamped);
	CHK(r == 1.0);
	CHK(atomic_load(&clamped) == 7);

	if (fails) return 1;
	printf("OK: source rate clamped to +-%d ppm, non-finite refused, clamps counted\n",
	       REAC_RATE_MATCH_MAX_PPM);
	return 0;
}
