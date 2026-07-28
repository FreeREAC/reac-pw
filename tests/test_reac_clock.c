// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The clock-discipline core (issue #75), offline. Everything the pacer will ever
 * decide about WHICH clock to follow and HOW HARD to pull toward it is pure, so
 * it is provable without a box:
 *
 *   1. the ROLE-DEPENDENT hierarchy — two distinct orderings, out-of-role
 *      references ignored, no ties, and a reference disappearing;
 *   2. the DLL — convergence, no overshoot, bounded correction, insane samples
 *      rejected rather than clamped in, phase never stepped;
 *   3. honest degradation — free-run is never called locked, and a vanished
 *      reference holds the last good rate instead of snapping;
 *   4. a replay of a captured-shape counter series (a real reference has noise;
 *      the loop must not chase it).
 */
#include "reac_clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* 96 kHz downstream = 8000 fps. */
#define NOMINAL_NS 125000L

int main(void)
{
	/* ---- 1. the hierarchy ------------------------------------------------ */

	/* The MASTER ordering, asserted as data, not inferred from the selector. */
	{
		int n = 0;
		const enum reac_clock_source *h = reac_clock_hierarchy(REAC_ROLE_MASTER, &n);
		CHK(n == 3);
		CHK(h[0] == REAC_CLOCK_SRC_PHC);
		CHK(h[1] == REAC_CLOCK_SRC_GRAPH);
		CHK(h[2] == REAC_CLOCK_SRC_BOX);
	}
	/* The SLAVE ordering: the wire, and nothing else. */
	{
		int n = 0;
		const enum reac_clock_source *h = reac_clock_hierarchy(REAC_ROLE_SLAVE, &n);
		CHK(n == 1);
		CHK(h[0] == REAC_CLOCK_SRC_WIRE);
	}

	/* MASTER: best-available across EVERY combination of the three candidates —
	 * all 8 subsets, so the ordering is pinned exhaustively and "ties" (two or
	 * three present at once) resolve by the list, never by arrival order. */
	{
		const uint32_t P = REAC_CLOCK_AVAIL_PHC;
		const uint32_t G = REAC_CLOCK_AVAIL_GRAPH;
		const uint32_t B = REAC_CLOCK_AVAIL_BOX;
		CHK(reac_clock_select(REAC_ROLE_MASTER, 0)         == REAC_CLOCK_SRC_FREERUN);
		CHK(reac_clock_select(REAC_ROLE_MASTER, B)         == REAC_CLOCK_SRC_BOX);
		CHK(reac_clock_select(REAC_ROLE_MASTER, G)         == REAC_CLOCK_SRC_GRAPH);
		CHK(reac_clock_select(REAC_ROLE_MASTER, P)         == REAC_CLOCK_SRC_PHC);
		CHK(reac_clock_select(REAC_ROLE_MASTER, G | B)     == REAC_CLOCK_SRC_GRAPH);
		CHK(reac_clock_select(REAC_ROLE_MASTER, P | B)     == REAC_CLOCK_SRC_PHC);
		CHK(reac_clock_select(REAC_ROLE_MASTER, P | G)     == REAC_CLOCK_SRC_PHC);
		CHK(reac_clock_select(REAC_ROLE_MASTER, P | G | B) == REAC_CLOCK_SRC_PHC);
	}

	/* A MASTER never treats the incoming master cadence as a reference — there is
	 * no other master on our segment, and the bit is not in its hierarchy. */
	CHK(reac_clock_select(REAC_ROLE_MASTER, REAC_CLOCK_AVAIL_WIRE)
	    == REAC_CLOCK_SRC_FREERUN);

	/* SLAVE: the wire is non-negotiable. A PHC and a hardware-driven graph clock
	 * in the same host do NOT displace it. */
	CHK(reac_clock_select(REAC_ROLE_SLAVE, REAC_CLOCK_AVAIL_WIRE) == REAC_CLOCK_SRC_WIRE);
	CHK(reac_clock_select(REAC_ROLE_SLAVE,
	                      REAC_CLOCK_AVAIL_WIRE | REAC_CLOCK_AVAIL_PHC |
	                      REAC_CLOCK_AVAIL_GRAPH | REAC_CLOCK_AVAIL_BOX)
	    == REAC_CLOCK_SRC_WIRE);
	/* ...and with no master cadence it names free-run honestly rather than
	 * claiming a local clock disciplines its upstream. */
	CHK(reac_clock_select(REAC_ROLE_SLAVE,
	                      REAC_CLOCK_AVAIL_PHC | REAC_CLOCK_AVAIL_GRAPH)
	    == REAC_CLOCK_SRC_FREERUN);

	/* Graph-reference admission: a `clock.system.*` driver is timed by
	 * CLOCK_MONOTONIC itself — following it is following our own free-run. */
	CHK(reac_clock_name_is_hardware("api.alsa.pcm.sink") == 1);
	CHK(reac_clock_name_is_hardware("clock.system.monotonic") == 0);
	CHK(reac_clock_name_is_hardware("") == 0);
	CHK(reac_clock_name_is_hardware(NULL) == 0);

	/* rate_diff -> ppm, and the "not measured yet" guard. */
	CHK(fabs(reac_clock_ppm_from_rate_diff(1.0)) < 1e-9);
	CHK(fabs(reac_clock_ppm_from_rate_diff(1.00005) - 50.0) < 1e-6);
	CHK(fabs(reac_clock_ppm_from_rate_diff(0.99995) + 50.0) < 1e-6);
	CHK(fabs(reac_clock_ppm_from_rate_diff(0.0)) < 1e-9);

	/* ---- 2. the DLL ------------------------------------------------------ */

	/* Period math: nominal at zero correction, and reciprocal in the right
	 * direction — a reference running FAST shortens our period. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		CHK(reac_dll_period_ns(&d) == NOMINAL_NS);
		CHK(reac_dll_applied_ppm(&d) == 0.0);
	}

	/* CONVERGENCE + NO OVERSHOOT: a single-pole loop approaches the measurement
	 * monotonically and never crosses it. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		const double m = 37.0;
		double prev = 0.0;
		for (int i = 0; i < 400; i++) {
			CHK(reac_dll_update(&d, m) == 1);
			double a = reac_dll_applied_ppm(&d);
			CHK(a >= prev - 1e-12);     /* monotonic */
			CHK(a <= m + 1e-9);         /* never overshoots the target */
			prev = a;
		}
		CHK(fabs(reac_dll_applied_ppm(&d) - m) < 0.01);
		/* and the steered period is the reciprocal of the reference's rate */
		long want = (long)((double)NOMINAL_NS / (1.0 + m / 1e6) + 0.5);
		CHK(reac_dll_period_ns(&d) == want);
		CHK(reac_dll_period_ns(&d) < NOMINAL_NS);   /* fast reference -> shorter */
	}
	/* symmetric for a slow reference */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 400; i++)
			reac_dll_update(&d, -37.0);
		CHK(fabs(reac_dll_applied_ppm(&d) + 37.0) < 0.01);
		CHK(reac_dll_period_ns(&d) > NOMINAL_NS);
	}

	/* SLEW BOUND: a step in the measurement is followed as a glide, never a jump.
	 * This is what keeps the correction inaudible. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		double prev = 0.0;
		for (int i = 0; i < 200; i++) {
			reac_dll_update(&d, 150.0);
			double a = reac_dll_applied_ppm(&d);
			CHK(fabs(a - prev) <= REAC_DLL_SLEW_PPM + 1e-9);
			prev = a;
		}
		CHK(fabs(reac_dll_applied_ppm(&d) - 150.0) < 0.01);
		CHK(d.clamped == 0);
	}

	/* BOUNDED CORRECTION: a reference that is plausible but far outside any real
	 * audio clock saturates at MAX_PPM and stays there. The cadence cannot run
	 * away. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 2000; i++)
			reac_dll_update(&d, 1900.0);          /* inside SANE, way past MAX */
		CHK(fabs(reac_dll_applied_ppm(&d) - REAC_DLL_MAX_PPM) < 1e-9);
		CHK(d.clamped > 0);
		long p = reac_dll_period_ns(&d);
		long floor_ns = (long)((double)NOMINAL_NS / (1.0 + REAC_DLL_MAX_PPM / 1e6));
		CHK(p >= floor_ns);
		CHK(p >= NOMINAL_NS - NOMINAL_NS / 4000);  /* < 250 ppm off nominal */
	}

	/* INSANE SAMPLES ARE REJECTED, NOT CLAMPED IN: garbage must contribute
	 * nothing at all — not even one slew step. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		CHK(reac_dll_update(&d, 1e6) == 0);
		CHK(reac_dll_update(&d, -1e6) == 0);
		CHK(reac_dll_update(&d, NAN) == 0);        /* the !(a<x && a>y) form catches it */
		CHK(reac_dll_applied_ppm(&d) == 0.0);
		CHK(reac_dll_period_ns(&d) == NOMINAL_NS);
		CHK(d.rejected == 3);
		CHK(d.updates == 0);
	}
	/* a burst of garbage in the middle of a good run leaves the cadence untouched */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 200; i++)
			reac_dll_update(&d, 20.0);
		double good = reac_dll_applied_ppm(&d);
		long good_ns = reac_dll_period_ns(&d);
		for (int i = 0; i < 50; i++)
			CHK(reac_dll_update(&d, 500000.0) == 0);
		CHK(reac_dll_applied_ppm(&d) == good);
		CHK(reac_dll_period_ns(&d) == good_ns);
	}

	/* ---- 3. the discipline: honest states -------------------------------- */

	/* No reference, ever: free-run, and NEVER a lock claim. */
	{
		struct reac_clock_disc c;
		char buf[128];
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		for (int i = 0; i < 50; i++)
			reac_clock_disc_update(&c, 0, 12.0, 1);   /* a ppm nobody vouches for */
		CHK(c.src == REAC_CLOCK_SRC_FREERUN);
		CHK(c.state == REAC_CLOCK_UNLOCKED);
		CHK(reac_clock_disc_period_ns(&c) == NOMINAL_NS);
		CHK(strcmp(reac_clock_disc_describe(&c, buf, sizeof buf),
		           "master (pace: generated here) — free-running (no reference)")
		    == 0);
		CHK(strstr(buf, "locked to") == NULL);   /* never a lock claim */
		CHK(strstr(buf, "holdover") == NULL);    /* nor a held-rate claim */
	}

	/* A reference appears: LOCKING then LOCKED, and the transcript names both the
	 * pace and the reference. */
	{
		struct reac_clock_disc c;
		char buf[128];
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 25.0, 1);
		CHK(c.src == REAC_CLOCK_SRC_GRAPH);
		CHK(c.state == REAC_CLOCK_LOCKING);
		CHK(c.generation == 1);                      /* the change is reportable */
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 25.0, 1);
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(fabs(reac_dll_applied_ppm(&c.dll) - 25.0) < 0.01);
		CHK(strcmp(reac_clock_disc_describe(&c, buf, sizeof buf),
		           "master (pace: generated here) — locked to graph clock") == 0);

		/* THE REFERENCE VANISHES MID-RUN: hold the last good rate, report
		 * holdover, do NOT snap back to nominal and do NOT keep claiming lock. */
		long held = reac_clock_disc_period_ns(&c);
		uint32_t gen = c.generation;
		reac_clock_disc_update(&c, 0, 0.0, 0);
		CHK(c.src == REAC_CLOCK_SRC_FREERUN);
		CHK(c.state == REAC_CLOCK_HOLDOVER);
		CHK(c.generation > gen);
		CHK(c.holdovers == 1);
		CHK(reac_clock_disc_period_ns(&c) == held);
		CHK(held != NOMINAL_NS);
		for (int i = 0; i < 100; i++)
			reac_clock_disc_update(&c, 0, 0.0, 0);
		CHK(reac_clock_disc_period_ns(&c) == held);  /* stays held, no drift back */
		CHK(c.holdovers == 1);                       /* one event, not one per tick */

		/* It comes back: we resume from the HELD rate, not from nominal. */
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 25.0, 1);
		CHK(c.src == REAC_CLOCK_SRC_GRAPH);
		CHK(c.state == REAC_CLOCK_LOCKING);           /* re-earn the lock claim */
		CHK(fabs(reac_dll_applied_ppm(&c.dll) - 25.0) < 0.1);
	}

	/* A better reference arrives mid-run: we switch, and re-earn LOCKED. */
	{
		struct reac_clock_disc c;
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 10.0, 1);
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(c.switches == 1);
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH | REAC_CLOCK_AVAIL_PHC,
		                       -8.0, 1);
		CHK(c.src == REAC_CLOCK_SRC_PHC);
		CHK(c.switches == 2);
		CHK(c.state == REAC_CLOCK_LOCKING);
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH | REAC_CLOCK_AVAIL_PHC,
			                       -8.0, 1);
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(fabs(reac_dll_applied_ppm(&c.dll) + 8.0) < 0.01);
	}

	/* A present-but-silent reference is not a vanished one: no holdover event, no
	 * lock claim, cadence unchanged. */
	{
		struct reac_clock_disc c;
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		for (int i = 0; i < 20; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_PHC, 0.0, 0);
		CHK(c.src == REAC_CLOCK_SRC_PHC);
		CHK(c.state == REAC_CLOCK_LOCKING);
		CHK(c.holdovers == 0);
		CHK(reac_clock_disc_period_ns(&c) == NOMINAL_NS);
	}

	/* A reference that only ever emits garbage never earns a lock claim. */
	{
		struct reac_clock_disc c;
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		for (int i = 0; i < 100; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 900000.0, 1);
		CHK(c.state == REAC_CLOCK_LOCKING);
		CHK(c.dll.rejected == 100);
		CHK(reac_clock_disc_period_ns(&c) == NOMINAL_NS);
	}

	/* SLAVE role, wired end to end through the discipline: the wire is the
	 * reference and the transcript says the pace is an INPUT. */
	{
		struct reac_clock_disc c;
		char buf[128];
		reac_clock_disc_init(&c, REAC_ROLE_SLAVE, NOMINAL_NS);
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c,
			                       REAC_CLOCK_AVAIL_WIRE | REAC_CLOCK_AVAIL_PHC,
			                       14.0, 1);
		CHK(c.src == REAC_CLOCK_SRC_WIRE);
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(strcmp(reac_clock_disc_describe(&c, buf, sizeof buf),
		           "slave (pace: from the master) — locked to master cadence") == 0);
		/* the master goes away: holdover on the master's last known rate */
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_PHC, 0.0, 0);
		CHK(c.src == REAC_CLOCK_SRC_FREERUN);
		CHK(c.state == REAC_CLOCK_HOLDOVER);
	}

	/* ---- 4. replay: a noisy reference must not be chased ----------------- *
	 * The shape of a real counter-slope series (reac_rx recomputes ~4x/s): a
	 * constant offset plus per-window jitter of a few ppm. The loop must settle on
	 * the OFFSET and the period must stop moving meaningfully — a per-sample
	 * chase would smear the cadence the pacer exists to protect. */
	{
		struct reac_clock_disc c;
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		const double offset = 31.0;
		/* deterministic pseudo-noise, +-3 ppm, no rand() dependency */
		uint32_t s = 12345u;
		long last = 0;
		int big_moves = 0;
		for (int i = 0; i < 4000; i++) {
			s = s * 1103515245u + 12345u;
			double jitter = ((double)((s >> 16) & 0xffff) / 65535.0 - 0.5) * 6.0;
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, offset + jitter, 1);
			long p = reac_clock_disc_period_ns(&c);
			if (i > 200 && labs(p - last) > 1)   /* ns: 1 ns @125 us = 8 ppm */
				big_moves++;
			last = p;
		}
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(fabs(reac_dll_applied_ppm(&c.dll) - offset) < 1.5);
		CHK(big_moves == 0);
	}

	printf("test_reac_clock: OK\n");
	return 0;
}
