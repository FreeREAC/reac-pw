// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_envflag + the DEFAULTS the boolean knobs ship with.
 *
 * Two things are pinned here and they are different claims:
 *
 *   (a) ONE READING FOR EVERY KNOB. Three knobs had three spellings of the same
 *       question, and the worst of them read REACPW_CLOCK_FOLLOW=0 as "on" because
 *       it only asked whether the variable was SET. An operator who types 0 has
 *       said no; a reader that hears yes is a trap, not a knob.
 *
 *   (b) THE SHIPPED DEFAULT OF THE CLOCK DISCIPLINE IS ON (0.5.0, after the rig ran
 *       follow=1 + REF=Babyface from 2026-09-07 20:55 without incident). The constant
 *       is asserted directly, so
 *       flipping REAC_CLOCK_FOLLOW_DEFAULT back to 0 turns this test red — the
 *       default is not something a later edit can move quietly.
 *
 * What this CANNOT prove is that main.c passes that constant rather than a literal;
 * that is one line, `.clock_follow = reac_envflag("REACPW_CLOCK_FOLLOW",
 * REAC_CLOCK_FOLLOW_DEFAULT)`, and the journal line the pacer prints on a real start
 * ("following ENABLED (the default; ...)") is what witnesses it on the rig. */
#include "reac_envflag.h"
#include "reac_sink_node.h"   /* REAC_CLOCK_FOLLOW_DEFAULT */

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	/* ---- (a) the parse. Absence and emptiness are NOT answers. */
	CHK(reac_envflag_parse(NULL) == -1);
	CHK(reac_envflag_parse("") == -1);
	/* A key someone blanked out is a key they turned off, not one set to "" —
	 * reac_conf.h's rule, and the same rule here. */
	CHK(reac_envflag("REAC_TEST_FLAG_UNSET", 1) == 1);
	CHK(reac_envflag("REAC_TEST_FLAG_UNSET", 0) == 0);

	/* Every spelling of no, and of yes, in either case. */
	const char *no[]  = { "0", "n", "N", "no", "NO", "f", "false", "FALSE", "off", "Off" };
	const char *yes[] = { "1", "y", "Y", "yes", "YES", "t", "true", "TRUE", "on", "ON" };
	for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++)
		CHK(reac_envflag_parse(no[i]) == 0);
	for (unsigned i = 0; i < sizeof yes / sizeof yes[0]; i++)
		CHK(reac_envflag_parse(yes[i]) == 1);

	/* A value nobody can parse is not consent: it falls back to the default rather
	 * than being taken as true because it is non-empty. */
	CHK(reac_envflag_parse("maybe") == -1);
	CHK(reac_envflag_parse("2") == -1);

	/* ---- the env door itself, both directions, on the knob that changed. */
	setenv("REACPW_CLOCK_FOLLOW", "0", 1);
	CHK(reac_envflag("REACPW_CLOCK_FOLLOW", REAC_CLOCK_FOLLOW_DEFAULT) == 0);
	setenv("REACPW_CLOCK_FOLLOW", "1", 1);
	CHK(reac_envflag("REACPW_CLOCK_FOLLOW", REAC_CLOCK_FOLLOW_DEFAULT) == 1);
	/* THE REGRESSION THIS KNOB HAD: `=0` used to enable it, because the reader asked
	 * whether the variable existed. Keep both arms — an assertion that only ever sees
	 * the true arm cannot see that failure. */
	setenv("REACPW_CLOCK_FOLLOW", "0", 1);
	CHK(reac_envflag("REACPW_CLOCK_FOLLOW", REAC_CLOCK_FOLLOW_DEFAULT) == 0);
	unsetenv("REACPW_CLOCK_FOLLOW");

	/* ---- (b) the default itself. Unset means FOLLOW. */
	CHK(REAC_CLOCK_FOLLOW_DEFAULT == 1);
	CHK(reac_envflag("REACPW_CLOCK_FOLLOW", REAC_CLOCK_FOLLOW_DEFAULT) == 1);

	/* Rate matching still ships OFF, and for its own measured reason (its loop's
	 * measurement phase is wrong at rest). One shared reader must not drag every
	 * knob to the same default. */
	CHK(reac_envflag("REACPW_RATE_MATCH", 0) == 0);
	setenv("REACPW_RATE_MATCH", "1", 1);
	CHK(reac_envflag("REACPW_RATE_MATCH", 0) == 1);
	unsetenv("REACPW_RATE_MATCH");

	printf("ok: one boolean reader; the clock discipline follows by default, "
	       "REACPW_CLOCK_FOLLOW=0 opts out\n");
	return 0;
}
