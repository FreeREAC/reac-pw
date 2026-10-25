// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A MASTER THAT STREAMS AT NOBODY STOPS (#40, reac_empty_master.h), with values, at main.c's
 * 200 ms poll. The first cut stopped a master 10 s after three facts agreed, and one of them
 * (the RX latch) took 5 s of its own: up to 15 s of ~96 Mbit/s at an empty wire.
 *
 *   1. A courting master whose box stops at +5.0 s stops at the first poll at or past
 *      +7.0 s, and not one poll before.
 *   2. A box that streams for a minute is never stopped.
 *   3. An enrolled box rides through 6 s of silence while libreac's master holds it; the
 *      poll that sees the master back in PROBING stops the stream.
 *   4. A master that opened on a wire where no box frame ever arrives stops at +2.0 s.
 *
 * Sabotage recorded in the PR: REAC_EMPTY_MASTER_QUIET_NS at 10 s turns 1 and 4 red; dropping
 * the `was_past` branch turns 3 red. */
#include "reac_empty_master.h"

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL
#define POLL (200ULL * MS)

/* Polls from t0 until it says stop or `until`; returns the stop time or 0. `rx(t)` is the
 * pacer's box-frame count at t, `past(t)` the master's state. */
typedef uint64_t (*rx_fn)(uint64_t t);
typedef int (*past_fn)(uint64_t t);

static uint64_t T0;

static uint64_t poll_until(rx_fn rx, past_fn past, uint64_t until)
{
	struct reac_empty_master e;
	reac_empty_master_init(&e, T0, rx(T0));
	for (uint64_t t = T0 + POLL; t <= T0 + until; t += POLL)
		if (reac_empty_master_step(&e, t, rx(t), past(t)))
			return t;
	return 0;
}

/* 8000 box frames a second until +5 s, then nothing */
static uint64_t rx_stops_at_5(uint64_t t) { uint64_t d = t - T0; return (d < 5 * SEC ? d : 5 * SEC) / 125000ULL; }
static uint64_t rx_streams(uint64_t t)    { return (t - T0) / 125000ULL; }
static uint64_t rx_never(uint64_t t)      { (void)t; return 0; }
static int courting(uint64_t t)           { (void)t; return 0; }
/* enrolled from +1 s; the box falls silent at +3 s; libreac holds it until +9.4 s */
static uint64_t rx_enrolled(uint64_t t)   { uint64_t d = t - T0; return (d < 3 * SEC ? d : 3 * SEC) / 125000ULL; }
static int held_until_9_4(uint64_t t)     { uint64_t d = t - T0; return d >= 1 * SEC && d < 9400 * MS; }

int main(void)
{
	T0 = 1000ULL * SEC;
	uint64_t s;

	s = poll_until(rx_stops_at_5, courting, 60 * SEC);
	fprintf(stderr, "1: box silent from +5.000 s: stopped at +%.3f s\n",
	        s ? (double)(s - T0) / 1e9 : -1.0);
	CHK(s == T0 + 7000 * MS);

	s = poll_until(rx_streams, courting, 60 * SEC);
	fprintf(stderr, "2: box streaming for 60 s: %s\n", s ? "STOPPED" : "never stopped");
	CHK(s == 0);

	s = poll_until(rx_enrolled, held_until_9_4, 60 * SEC);
	fprintf(stderr, "3: enrolled box silent from +3.000 s, libreac gives it up at +9.400 s: "
	        "stopped at +%.3f s\n", s ? (double)(s - T0) / 1e9 : -1.0);
	CHK(s == T0 + 9400 * MS);

	s = poll_until(rx_never, courting, 60 * SEC);
	fprintf(stderr, "4: no box frame ever: stopped at +%.3f s\n",
	        s ? (double)(s - T0) / 1e9 : -1.0);
	CHK(s == T0 + 2000 * MS);

	printf("OK: a master with no box stops within %.1f s of its last box frame, and at once "
	       "when libreac gives an enrolled box up\n", (double)REAC_EMPTY_MASTER_QUIET_NS / 1e9);
	return 0;
}
