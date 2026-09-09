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
#include <reac/reac_clock.h>

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

	/* ---- 5. reference QUALITY (#77) -------------------------------------- *
	 * The GRAPH tier used to ask one question — "is this hardware?" — and admit
	 * anything that answered yes. Owning a REAC segment's pace with a display
	 * clock is jitter propagated with authority, so the tier now also asks
	 * whether the thing is fit for the job. Every rule below exists to make that
	 * refinement safe: the heuristic may only REJECT, the operator outranks the
	 * heuristic, and the MEASUREMENT outranks both. */

	/* The ladder is an ORDER, and UNGRADED deliberately sits ABOVE MARGINAL:
	 * "no evidence" must never be worse than an earned demotion. */
	CHK(REAC_CLOCK_Q_UNUSABLE < REAC_CLOCK_Q_MARGINAL);
	CHK(REAC_CLOCK_Q_MARGINAL < REAC_CLOCK_Q_UNGRADED);
	CHK(REAC_CLOCK_Q_UNGRADED < REAC_CLOCK_Q_GOOD);
	CHK(REAC_CLOCK_Q_GOOD     < REAC_CLOCK_Q_DESIGNATED);

	/* Eligibility to OWN a segment: UNGRADED qualifies — refusing an operator
	 * their rig because we have never heard of their converter is the
	 * vendor-allow-list failure wearing a different hat. MARGINAL does not: that
	 * verdict was measured. */
	CHK(reac_clock_quality_can_own(REAC_CLOCK_Q_UNGRADED)   == 1);
	CHK(reac_clock_quality_can_own(REAC_CLOCK_Q_GOOD)       == 1);
	CHK(reac_clock_quality_can_own(REAC_CLOCK_Q_DESIGNATED) == 1);
	CHK(reac_clock_quality_can_own(REAC_CLOCK_Q_MARGINAL)   == 0);
	CHK(reac_clock_quality_can_own(REAC_CLOCK_Q_UNUSABLE)   == 0);

	/* THE NAME HEURISTIC MAY ONLY REJECT. What it rejects, it rejects for what
	 * the clock IS, never for who made it: software timers, and sinks whose word
	 * clock descends from a pixel clock. */
	CHK(reac_clock_name_quality("clock.system.monotonic") == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("Dummy-Driver")           == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("Freewheel-Driver")       == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("alsa_output.pci-0000_01_00.1.hdmi-stereo")
	    == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("HDMI 1")                 == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("Built-in Audio DisplayPort 3")
	    == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality(NULL)                     == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_name_quality("")                       == REAC_CLOCK_Q_UNUSABLE);

	/* ...and it promotes NOTHING. The RME the operator bought for its PLL grades
	 * exactly the same as an interface nobody here has ever heard of. That
	 * identity IS the no-allow-list rule — if this ever stops holding, someone
	 * has started demoting hardware for being unfamiliar. */
	CHK(reac_clock_name_quality("api.alsa.pcm.sink") == REAC_CLOCK_Q_UNGRADED);
	CHK(reac_clock_name_quality("alsa_output.usb-RME_Babyface_Pro-00.pro-output-0")
	    == REAC_CLOCK_Q_UNGRADED);
	CHK(reac_clock_name_quality("Obscure Pro Converter Mk4")
	    == REAC_CLOCK_Q_UNGRADED);
	CHK(reac_clock_name_quality("Built-in Audio Analog Stereo")
	    == REAC_CLOCK_Q_UNGRADED);

	/* Designation: case-insensitive SUBSTRING, and strictly opt-in — an empty
	 * designation must never quietly promote the first device to appear. */
	CHK(reac_clock_name_is_designated(
	        "alsa_output.usb-RME_Babyface_Pro-00.pro-output-0", "babyface") == 1);
	CHK(reac_clock_name_is_designated("RME Babyface Pro", "BABYFACE") == 1);
	CHK(reac_clock_name_is_designated("RME Babyface Pro", "Fireface") == 0);
	CHK(reac_clock_name_is_designated("RME Babyface Pro", "")   == 0);
	CHK(reac_clock_name_is_designated("RME Babyface Pro", NULL) == 0);
	CHK(reac_clock_name_is_designated(NULL, "babyface")         == 0);

	/* The publisher's whole inferred verdict: designation promotes an ungraded
	 * device, and does NOT rescue a disqualified one. An operator may choose
	 * among plausible references; they may not designate a pixel clock into a
	 * word clock. */
	CHK(reac_clock_grade_name("RME Babyface Pro", "babyface")
	    == REAC_CLOCK_Q_DESIGNATED);
	CHK(reac_clock_grade_name("RME Babyface Pro", NULL) == REAC_CLOCK_Q_UNGRADED);
	CHK(reac_clock_grade_name("Built-in Audio HDMI 1", "hdmi")
	    == REAC_CLOCK_Q_UNUSABLE);
	CHK(reac_clock_grade_name("Dummy-Driver", "dummy") == REAC_CLOCK_Q_UNUSABLE);

	/* MEASURED BEATS INFERRED — the whole table, every asymmetry as one case. */
	{
		const enum reac_clock_source G = REAC_CLOCK_SRC_GRAPH;
		const enum reac_clock_source B = REAC_CLOCK_SRC_BOX;
		/* structural rejection is sticky against BOTH overrides */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_UNUSABLE,
		                             REAC_CLOCK_STAB_STABLE) == REAC_CLOCK_Q_UNUSABLE);
		/* measured instability demotes an unremarkable device... */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_UNGRADED,
		                             REAC_CLOCK_STAB_UNSTABLE) == REAC_CLOCK_Q_MARGINAL);
		/* ...and a DESIGNATED one too. Designation outranks the heuristic, not
		 * the evidence; an operator whose reference wanders needs telling. */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_DESIGNATED,
		                             REAC_CLOCK_STAB_UNSTABLE) == REAC_CLOCK_Q_MARGINAL);
		/* measured stability is a PROMOTION, earned */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_UNGRADED,
		                             REAC_CLOCK_STAB_STABLE) == REAC_CLOCK_Q_GOOD);
		/* but never above a designation, which is already the top */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_DESIGNATED,
		                             REAC_CLOCK_STAB_STABLE) == REAC_CLOCK_Q_DESIGNATED);
		/* NEVER for the BOX tier: a box slaved to us hands our own correction
		 * back, so a perfect residual there is perfectly meaningless */
		CHK(reac_clock_quality_apply(B, REAC_CLOCK_Q_UNGRADED,
		                             REAC_CLOCK_STAB_STABLE) == REAC_CLOCK_Q_UNGRADED);
		/* ...while a box that genuinely wanders is still demoted */
		CHK(reac_clock_quality_apply(B, REAC_CLOCK_Q_UNGRADED,
		                             REAC_CLOCK_STAB_UNSTABLE) == REAC_CLOCK_Q_MARGINAL);
		/* no evidence changes nothing, in either direction */
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_UNGRADED,
		                             REAC_CLOCK_STAB_UNKNOWN) == REAC_CLOCK_Q_UNGRADED);
		CHK(reac_clock_quality_apply(G, REAC_CLOCK_Q_MARGINAL,
		                             REAC_CLOCK_STAB_UNKNOWN) == REAC_CLOCK_Q_MARGINAL);
	}

	/* The ADMISSION BAR skips a disqualified candidate and carries on DOWN the
	 * same list — the tier ordering is untouched by any of this. */
	{
		enum reac_clock_quality q[REAC_CLOCK_SRC_COUNT];
		for (int i = 0; i < REAC_CLOCK_SRC_COUNT; i++)
			q[i] = REAC_CLOCK_Q_UNGRADED;
		const uint32_t GB = REAC_CLOCK_AVAIL_GRAPH | REAC_CLOCK_AVAIL_BOX;
		/* an HDMI sink is the elected driver: we fall THROUGH to the box, not to
		 * free-run, and certainly not onto the display clock */
		q[REAC_CLOCK_SRC_GRAPH] = REAC_CLOCK_Q_UNUSABLE;
		CHK(reac_clock_select_graded(REAC_ROLE_MASTER, GB, q,
		                             REAC_CLOCK_Q_MARGINAL) == REAC_CLOCK_SRC_BOX);
		/* nothing left that clears the bar -> honest free-run */
		q[REAC_CLOCK_SRC_BOX] = REAC_CLOCK_Q_UNUSABLE;
		CHK(reac_clock_select_graded(REAC_ROLE_MASTER, GB, q,
		                             REAC_CLOCK_Q_MARGINAL) == REAC_CLOCK_SRC_FREERUN);
		/* a MARGINAL graph clock still outranks a healthy box at the default bar:
		 * we flag rather than eject (see the header — ejecting flaps a live
		 * segment on our own opinion), and a raised bar is how a caller opts in */
		q[REAC_CLOCK_SRC_GRAPH] = REAC_CLOCK_Q_MARGINAL;
		q[REAC_CLOCK_SRC_BOX]   = REAC_CLOCK_Q_UNGRADED;
		CHK(reac_clock_select_graded(REAC_ROLE_MASTER, GB, q,
		                             REAC_CLOCK_Q_MARGINAL) == REAC_CLOCK_SRC_GRAPH);
		CHK(reac_clock_select_graded(REAC_ROLE_MASTER, GB, q,
		                             REAC_CLOCK_Q_UNGRADED) == REAC_CLOCK_SRC_BOX);
		/* and the ungraded selector is the graded one with nothing excluded */
		CHK(reac_clock_select_graded(REAC_ROLE_MASTER, GB, NULL,
		                             REAC_CLOCK_Q_MARGINAL)
		    == reac_clock_select(REAC_ROLE_MASTER, GB));
	}

	/* ---- 6. the MEASURED stability signal -------------------------------- *
	 * The part that matters most: a claim from the device's badge is a guess, the
	 * variance of the loop's own residual is evidence. */

	/* A rock-steady reference EARNS its grade — and only after enough evidence. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_UNKNOWN);
		for (int i = 0; i < 400; i++)
			reac_dll_update(&d, 12.0);
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_STABLE);
		CHK(reac_dll_resid_sigma(&d) < REAC_DLL_STABLE_PPM);
		CHK(reac_clock_quality_apply(REAC_CLOCK_SRC_GRAPH, REAC_CLOCK_Q_UNGRADED,
		                             reac_dll_stability(&d)) == REAC_CLOCK_Q_GOOD);
	}

	/* ACQUISITION IS NOT INSTABILITY. A big step followed as a slew-limited glide
	 * is a healthy loop doing its job, and its residual ramp has an enormous
	 * variance. If that ever counted, every good reference would be demoted for
	 * its first seconds — so assert the verdict is NEVER unstable, at any point. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 600; i++) {
			reac_dll_update(&d, 150.0);
			CHK(reac_dll_stability(&d) != REAC_CLOCK_STAB_UNSTABLE);
		}
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_STABLE);
	}

	/* OUR OWN ESTIMATOR'S NOISE IS NOT THE REFERENCE'S JITTER. reac_rx recomputes
	 * its counter slope ~4x/s and carries a couple of ppm of window noise, so a
	 * series at that level is INDISTINGUISHABLE from a perfect reference read
	 * through our own instrument. "We cannot tell" is the only honest verdict and
	 * the thresholds are spaced to give it: neither promoted nor demoted. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		uint32_t s = 12345u;
		for (int i = 0; i < 4000; i++) {
			s = s * 1103515245u + 12345u;
			double jitter = ((double)((s >> 16) & 0xffff) / 65535.0 - 0.5) * 6.0;
			reac_dll_update(&d, 31.0 + jitter);
			CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_UNKNOWN);
		}
	}

	/* A REFERENCE THAT WANDERS IS DEMOTED, whatever its badge says. Mostly quiet
	 * with periodic excursions: the mean residual stays small enough that the
	 * loop still reads as tracking, and only the VARIANCE gives it away — which
	 * is exactly why the variance is the signal and the mean is not. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 4000; i++)
			reac_dll_update(&d, (i % 40 == 0) ? 60.0 : 20.0);
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_UNSTABLE);
		CHK(reac_dll_resid_sigma(&d) > REAC_DLL_UNSTABLE_PPM);
		/* ...and that demotion survives an operator's designation, and takes the
		 * reference out of the "fit to own a segment" class. */
		enum reac_clock_quality q =
			reac_clock_quality_apply(REAC_CLOCK_SRC_GRAPH, REAC_CLOCK_Q_DESIGNATED,
			                         reac_dll_stability(&d));
		CHK(q == REAC_CLOCK_Q_MARGINAL);
		CHK(reac_clock_quality_can_own(q) == 0);
	}

	/* The accumulator belongs to ONE reference: a switch forgets the series, but
	 * NEVER the loop's memory — holdover is a separate promise and #77 does not
	 * touch it. */
	{
		struct reac_dll d;
		reac_dll_init(&d, NOMINAL_NS);
		for (int i = 0; i < 400; i++)
			reac_dll_update(&d, 12.0);
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_STABLE);
		double held = reac_dll_applied_ppm(&d);
		uint64_t seen = d.updates;
		reac_dll_stability_reset(&d);
		CHK(reac_dll_stability(&d) == REAC_CLOCK_STAB_UNKNOWN);
		CHK(reac_dll_applied_ppm(&d) == held);
		CHK(d.updates == seen);
		CHK(reac_dll_period_ns(&d) != NOMINAL_NS);
	}

	/* ---- 7. quality through the discipline, and in the transcript -------- */

	/* A structurally disqualified graph clock does not get to own the segment:
	 * the discipline falls through to the box exactly as if the driver were
	 * absent, and never once reports a lock to the display clock. */
	{
		struct reac_clock_disc c;
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		reac_clock_disc_set_quality(&c, REAC_CLOCK_SRC_GRAPH,
		                            reac_clock_name_quality("Built-in Audio HDMI 1"));
		reac_clock_disc_set_quality(&c, REAC_CLOCK_SRC_BOX, REAC_CLOCK_Q_UNGRADED);
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c,
			                       REAC_CLOCK_AVAIL_GRAPH | REAC_CLOCK_AVAIL_BOX,
			                       9.0, 1);
		CHK(c.src == REAC_CLOCK_SRC_BOX);
		CHK(c.state == REAC_CLOCK_LOCKED);
		/* and the box is never PROMOTED on a measurement (closed-loop caveat) */
		CHK(reac_clock_disc_quality(&c) == REAC_CLOCK_Q_UNGRADED);
	}

	/* A designated, measurably steady graph clock: the tier is reported, the
	 * change is a transcript event, and the line names device AND tier. */
	{
		struct reac_clock_disc c;
		char buf[224];
		reac_clock_disc_init(&c, REAC_ROLE_MASTER, NOMINAL_NS);
		/* ungraded until anyone says otherwise — the pre-#77 behaviour exactly */
		CHK(c.quality[REAC_CLOCK_SRC_GRAPH] == REAC_CLOCK_Q_UNGRADED);
		CHK(reac_clock_disc_quality(&c) == REAC_CLOCK_Q_UNGRADED);
		reac_clock_disc_set_quality(&c, REAC_CLOCK_SRC_GRAPH,
		                            reac_clock_grade_name("RME Babyface Pro",
		                                                  "babyface"));
		reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 18.0, 1);
		CHK(reac_clock_disc_quality(&c) == REAC_CLOCK_Q_DESIGNATED);
		uint32_t gen = c.generation;
		for (int i = 0; i < 500; i++)
			reac_clock_disc_update(&c, REAC_CLOCK_AVAIL_GRAPH, 18.0, 1);
		CHK(c.state == REAC_CLOCK_LOCKED);
		CHK(c.generation > gen);          /* the tier is never implicit */
		CHK(strcmp(reac_clock_describe_full(c.role, c.src, c.state,
		                                    reac_clock_disc_quality(&c),
		                                    "RME Babyface Pro", buf, sizeof buf),
		           "master (pace: generated here) — locked to graph clock "
		           "(RME Babyface Pro), quality: operator-designated") == 0);
	}

	/* The line degrades honestly: no device known, no opinion held, and NEITHER
	 * printed once there is no reference to name. */
	{
		char buf[224];
		CHK(strcmp(reac_clock_describe_full(REAC_ROLE_MASTER, REAC_CLOCK_SRC_GRAPH,
		                                    REAC_CLOCK_LOCKING,
		                                    REAC_CLOCK_Q_UNGRADED, NULL,
		                                    buf, sizeof buf),
		           "master (pace: generated here) — acquiring graph clock") == 0);
		CHK(strcmp(reac_clock_describe_full(REAC_ROLE_MASTER, REAC_CLOCK_SRC_GRAPH,
		                                    REAC_CLOCK_LOCKED,
		                                    REAC_CLOCK_Q_MARGINAL, "Onboard",
		                                    buf, sizeof buf),
		           "master (pace: generated here) — locked to graph clock "
		           "(Onboard), quality: marginal") == 0);
		CHK(strcmp(reac_clock_describe_full(REAC_ROLE_MASTER, REAC_CLOCK_SRC_FREERUN,
		                                    REAC_CLOCK_HOLDOVER,
		                                    REAC_CLOCK_Q_GOOD, "RME Babyface Pro",
		                                    buf, sizeof buf),
		           "master (pace: generated here) — holdover: reference lost, "
		           "holding the last good rate") == 0);
		/* and the three-field form is exactly this with neither */
		char a[224], b[224];
		reac_clock_describe(REAC_ROLE_SLAVE, REAC_CLOCK_SRC_WIRE,
		                    REAC_CLOCK_LOCKED, a, sizeof a);
		reac_clock_describe_full(REAC_ROLE_SLAVE, REAC_CLOCK_SRC_WIRE,
		                         REAC_CLOCK_LOCKED, REAC_CLOCK_Q_UNGRADED, NULL,
		                         b, sizeof b);
		CHK(strcmp(a, b) == 0);
	}

	printf("test_reac_clock: OK\n");
	return 0;
}
