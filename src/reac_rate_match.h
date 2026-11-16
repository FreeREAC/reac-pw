// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The resampler-rate arithmetic the source node hands PipeWire on the RT thread:
 * a measured ppm error in, the ratio for io_rate_match.rate out. Header-only and
 * pure (no PipeWire, no clock, no allocation, no logging) so a test can drive the
 * exact function the process callback calls. */
#ifndef REAC_RATE_MATCH_H
#define REAC_RATE_MATCH_H

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

/* Bound on the correction handed to PipeWire's resampler, in ppm. ONE constant: the
 * sink's level loop and the source's follower rate both read it. Sized so the sink's
 * level loop settles well inside its guard band (see reac_sink_node.c); 5000 ppm is
 * 0.007 of a semitone. A master measured outside it is not a clock to follow. */
#define REAC_RATE_MATCH_MAX_PPM  5000

/* ppm_milli is ppm * 1000 as libreac's feeder publishes it. The result is clamped to
 * 1 +- REAC_RATE_MATCH_MAX_PPM/1e6; a non-finite input is refused to nominal (1.0).
 * Either case bumps *clamped (relaxed; NULL allowed). */
static inline double reac_rate_match_from_ppm_milli(double ppm_milli,
                                                    _Atomic uint32_t *clamped)
{
	const double max_milli = (double)REAC_RATE_MATCH_MAX_PPM * 1000.0;
	if (!isfinite(ppm_milli)) {
		if (clamped) atomic_fetch_add_explicit(clamped, 1, memory_order_relaxed);
		return 1.0;
	}
	if (ppm_milli > max_milli || ppm_milli < -max_milli) {
		if (clamped) atomic_fetch_add_explicit(clamped, 1, memory_order_relaxed);
		ppm_milli = ppm_milli > 0 ? max_milli : -max_milli;
	}
	return 1.0 + ppm_milli / 1e9;
}

#endif /* REAC_RATE_MATCH_H */
