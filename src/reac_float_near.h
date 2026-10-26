/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_float_near: THE NUMBER A FLOAT CONTROL VALUE CARRIES.
 *
 * The three readers of SPA_PROP_params (reac_headamp_prop, reac_rate_cfg, reac_role_cfg)
 * take a value as Float as well as Int, because a slider or a JSON-number bridge sends
 * floats. The value a Float carries is its NEAREST INTEGER, a half read upward, and a
 * Float with no such integer in the range the reader takes (NaN, an infinity, a value
 * past either end) carries none: the reader refuses it as malformed.
 *
 * WHY ONE DOOR AND WHY IN DOUBLE. Each reader used to round with `(int)(f + 0.5f)`. In
 * float arithmetic 0.49999997f + 0.5f is 1.0f, so a phantom write of 0.49999997 switched
 * the phantom on and a role write of it asked for the slave engine; a negative fraction
 * truncated toward zero, so -0.7 read as 0, the master; and a NaN or a value past `int`
 * was converted unchecked, which is undefined. The sum in double is exact for every
 * float, so the floor below is the nearest integer the documentation promises. */
#ifndef REAC_FLOAT_NEAR_H
#define REAC_FLOAT_NEAR_H

/* The nearest integer to `f`, a half read upward, into *out when it lies in [lo, hi].
 * Returns 0, or -1 (and *out untouched) when there is none: NaN, an infinity, or a value
 * whose nearest integer is outside [lo, hi]. */
static inline int reac_float_nearest(float f, long long lo, long long hi, long long *out)
{
	const double d = (double)f + 0.5;
	if (!(d >= (double)lo && d < (double)hi + 1.0))
		return -1;
	long long r = (long long)d;   /* toward zero: one too high for a negative fraction */
	if ((double)r > d)
		r--;
	*out = r;
	return 0;
}

#endif /* REAC_FLOAT_NEAR_H */
