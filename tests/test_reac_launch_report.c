// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_launch_report: the words the daemon says about the hardware launch (reac-pw#98).
 * The health line is the operator's only view of whether the NIC or the kernel launches
 * the frames; "installed and is not on" is the line that says a silent fallback happened. */
#include "reac_launch_report.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void)
{
	char b[400];
	unsigned e = 7;

	/* ---- the knob: a frame count in [1, 1000000], the whole string */
	CHECK(reac_launch_stamps_parse("32", &e) == 1 && e == 32, "32 is a count");
	CHECK(reac_launch_stamps_parse("1", &e) == 1 && e == 1, "1 is a count");
	CHECK(reac_launch_stamps_parse("1000000", &e) == 1 && e == 1000000, "the upper bound is a count");
	e = 7;
	CHECK(reac_launch_stamps_parse("0", &e) == 0 && e == 7, "0 is refused and leaves the value");
	CHECK(reac_launch_stamps_parse("1000001", &e) == 0, "past the bound is refused");
	CHECK(reac_launch_stamps_parse("32x", &e) == 0, "trailing text is refused");
	CHECK(reac_launch_stamps_parse("", &e) == 0, "empty is refused");
	CHECK(reac_launch_stamps_parse("abc", &e) == 0, "not a number is refused");
	CHECK(reac_launch_stamps_parse(NULL, &e) == 0, "NULL is refused");
	CHECK(reac_launch_stamps_parse("-5", &e) == 0, "negative is refused");
	CHECK(e == 7, "no refusal touched the value");

	/* ---- arming refusals are named; armed and libreac's own errors are not */
	CHECK(!strcmp(reac_launch_stamps_refusal(-EINVAL) ? reac_launch_stamps_refusal(-EINVAL) : "", "invalid"), "EINVAL");
	CHECK(!strcmp(reac_launch_stamps_refusal(-EBUSY) ? reac_launch_stamps_refusal(-EBUSY) : "", "busy"), "EBUSY");
	CHECK(!strcmp(reac_launch_stamps_refusal(-ENOTSUP) ? reac_launch_stamps_refusal(-ENOTSUP) : "", "unsupported"), "ENOTSUP");
	CHECK(!strcmp(reac_launch_stamps_refusal(-EALREADY) ? reac_launch_stamps_refusal(-EALREADY) : "", "already armed"), "EALREADY");
	CHECK(!strcmp(reac_launch_stamps_refusal(-ENOMEM) ? reac_launch_stamps_refusal(-ENOMEM) : "", "out of memory"), "ENOMEM");
	CHECK(reac_launch_stamps_refusal(0) == NULL, "armed is not a refusal");
	CHECK(reac_launch_stamps_refusal(-EPERM) && !strcmp(reac_launch_stamps_refusal(-EPERM), "refused"),
	      "any other negative code is a refusal, never silent");
	CHECK(reac_launch_stamps_refusal(-9999) && !strcmp(reac_launch_stamps_refusal(-9999), "refused"),
	      "a code a later libreac adds is a refusal too");
	CHECK(reac_launch_stamps_refusal(1) == NULL, "a positive return is not a refusal");

	/* ---- the health line's launch words */
	reac_launch_health_line(b, sizeof b, "etf", "hardware", 1);
	CHECK(!strcmp(b, "etf, launch hardware"), "hardware installed and on: '%s'", b);
	reac_launch_health_line(b, sizeof b, "etf", "software", 0);
	CHECK(!strcmp(b, "etf, launch software"), "software by design says no alarm: '%s'", b);
	reac_launch_health_line(b, sizeof b, "etf", "software", 1);
	CHECK(!strcmp(b, "etf, launch software (THE HARDWARE FORM WAS INSTALLED AND IS NOT ON)"),
	      "installed but not on is said in capitals: '%s'", b);

	/* ---- the instrument of record */
	struct reac_launch_figures f = { .paired = 17736, .unpaired = 0, .outside = 0, .every = 32,
		.mean_ns = 25.6, .sd_ns = 25.9, .p50_ns = 22, .p99_ns = 70, .p999_ns = 80,
		.min_ns = -34, .max_ns = 88 };
	reac_launch_stamps_line(b, sizeof b, "enp1s0", &f);
	CHECK(!strcmp(b, " | launch-stamps [enp1s0] n=17736 (1/32, unpaired 0, outside +-10us 0) "
	                 "error mean +25.6 ns sd 25.9 ns p50 +22 p99 +70 p99.9 +80 min -34 max +88 ns"),
	      "launch-stamps line: '%s'", b);

	if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
	puts("OK: the launch words, the knob parse and the refusal names");
	return 0;
}
