// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_gain.h"

float reac_gain_ramp_block(float *buf, unsigned n, float cur, float target, float step)
{
	/* A non-positive step means "no ramp" — jump to the target immediately. */
	if (step <= 0.0f) {
		for (unsigned i = 0; i < n; i++)
			buf[i] *= target;
		return target;
	}

	for (unsigned i = 0; i < n; i++) {
		float d = target - cur;
		if (d > step)
			cur += step;        /* still climbing toward a louder target   */
		else if (d < -step)
			cur -= step;        /* still falling toward a quieter target    */
		else
			cur = target;       /* within one step — land exactly, no drift */
		buf[i] *= cur;
	}
	return cur;
}
