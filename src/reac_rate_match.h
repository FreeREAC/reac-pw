// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The resampler-rate arithmetic the source node hands PipeWire on the RT thread:
 * a measured ppm error in, the ratio for io_rate_match.rate out. Header-only and
 * pure (no PipeWire, no clock, no allocation, no logging) so a test can drive the
 * exact function the process callback calls. */
#ifndef REAC_RATE_MATCH_H
#define REAC_RATE_MATCH_H

#include <stdatomic.h>

#define REAC_RATE_MATCH_MAX_PPM  5000

/* ppm_milli is ppm * 1000 as libreac's feeder publishes it. */
static inline double reac_rate_match_from_ppm_milli(double ppm_milli,
                                                    _Atomic unsigned long long *clamped)
{
	(void)clamped;
	return 1.0 + ppm_milli / 1e9;
}

#endif /* REAC_RATE_MATCH_H */
