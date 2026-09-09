// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The pacer's clock-discipline WIRING (issue #75), offline. No socket, no RT
 * privilege, no PipeWire: it drives reac_pacer_clock_publish /
 * reac_pacer_clock_tick exactly as the publishers and the RT thread do, so both
 * the plumbing and the contract that matters most are exercised without hardware.
 *
 * The contract that matters most is INERTNESS. This lands without a rig, so the
 * default path must be provably unchanged:
 *   - with the knob unset, every tick returns the nominal period, whatever is
 *     published — including references wild enough to saturate the loop;
 *   - the discipline object is never advanced (no updates, no state change, no
 *     transcript event), so nothing about the emission or the log moves.
 *
 * Then the two references the rig actually uses, to the SAME standard:
 *   - GRAPH: the elected PipeWire driver (the RME on this rig) — the expected
 *     configuration, not a hypothetical;
 *   - BOX: a stagebox that is itself the clock master. reac-pw is the REAC master
 *     (it grants, it drives the segment) while being a clock FOLLOWER. That
 *     combination is the whole point of the issue and it gets its own coverage,
 *     including the closed-loop case where the box is slaved to US and its counter
 *     slope is merely our own pace handed back.
 */
#include "reac_pacer.h"
#include <reac/reac_clock.h>
#include <reac/reac_arbitration.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define NOMINAL_NS 125000L      /* 96 kHz downstream = 8000 fps */

/* Stand in for reac_pacer_open's clock half without touching a socket. */
static void pacer_clock_init(struct reac_pacer *p, int follow)
{
	memset(p, 0, sizeof *p);
	p->period_ns = NOMINAL_NS;
	p->fps = 8000;
	p->clock_follow = follow;
	p->slot_period_ns = p->period_ns;
	reac_clock_disc_init(&p->clock, REAC_ROLE_MASTER, p->period_ns);
}

/* Run one discipline evaluation: the tick is rate-limited to one per
 * REAC_CLOCK_TICK_SLOTS, so a caller that wants N evaluations runs N * that many
 * slots. Returns the period the last slot would have used. */
static long run_slots(struct reac_pacer *p, unsigned slots, uint64_t *now_ns)
{
	long period = 0;
	for (unsigned i = 0; i < slots; i++) {
		period = reac_pacer_clock_tick(p, *now_ns);
		*now_ns += (uint64_t)NOMINAL_NS;
	}
	return period;
}

int main(void)
{
	/* ---- 1. INERT BY DEFAULT --------------------------------------------- */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 0);              /* the default: knob unset */

		/* Publish everything, including a reference wild enough to saturate the
		 * loop, one that would be rejected outright, and (#77) graded references
		 * at both ends of the ladder — a designation must not wake the discipline
		 * up any more than a good ppm does. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 150000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_DESIGNATED,
			                         now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -900000,
			                         "S-1608", REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 1, 1000000000,
			                         "phc0", REAC_CLOCK_Q_UNGRADED, now);
			long period = run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
			/* Every slot advances the deadline by the nominal period — the exact
			 * constant a build without this code uses. */
			CHK(period == NOMINAL_NS);
		}
		/* ...and the discipline was never even consulted. */
		CHK(p.clock.dll.updates == 0);
		CHK(p.clock.dll.rejected == 0);
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);
		CHK(p.clock.state == REAC_CLOCK_UNLOCKED);
		CHK(p.clock.generation == 0);
		CHK(reac_clock_disc_quality(&p.clock) == REAC_CLOCK_Q_UNGRADED);
		CHK(p.slot_period_ns == NOMINAL_NS);
		/* No transcript event either: the log is byte-identical too. */
		CHK(atomic_load(&p.ev_head) == 0);
	}

	/* ---- 2. the label seqlock ------------------------------------------- */
	{
		struct reac_clock_label l;
		char out[REAC_CLOCK_LABEL_MAX];
		memset(&l, 0, sizeof l);
		CHK(reac_clock_label_set(&l, "RME Babyface Pro") == 1);
		CHK(reac_clock_label_set(&l, "RME Babyface Pro") == 0);  /* idempotent */
		CHK(reac_clock_label_get(&l, out, sizeof out) == 1);
		CHK(strcmp(out, "RME Babyface Pro") == 0);
		CHK(reac_clock_label_set(&l, NULL) == 0);                /* NULL = leave it */
		CHK(reac_clock_label_get(&l, out, sizeof out) == 1);
		CHK(strcmp(out, "RME Babyface Pro") == 0);
		/* A name longer than the slot truncates rather than overruns. */
		CHK(reac_clock_label_set(&l, "an absurdly long clock name that will not fit"));
		CHK(reac_clock_label_get(&l, out, sizeof out) == 1);
		CHK(strlen(out) == REAC_CLOCK_LABEL_MAX - 1);
		/* Mid-write (odd seq) reads as torn, so a half-written name never prints. */
		atomic_store(&l.seq, 7u);
		CHK(reac_clock_label_get(&l, out, sizeof out) == 0);
	}

	/* ---- 3. GRAPH: the elected driver, the expected rig configuration ---- */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);

		/* Nothing published yet: free-running, and it says so. */
		CHK(run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now) == NOMINAL_NS);
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);
		CHK(p.clock.state == REAC_CLOCK_UNLOCKED);
		CHK(reac_pacer_pace_source(&p) == REAC_PACE_FREE_RUN);

		/* The RME appears as the graph driver, running +18 ppm vs the host. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 18000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(p.slot_period_ns < NOMINAL_NS);        /* fast reference -> shorter */
		CHK(p.slot_period_ns == 124998L);          /* 125000 / 1.000018 */
		/* AND THE SEGMENT'S PUBLISHED PACE SAYS SO (0.5.4). This is the rig's own
		 * state, measured 2026-09-08/09: the journal read "locked to graph clock
		 * (api.alsa.0)" and reac.pace.source read "free-run", because the playback
		 * door built its arbitration with the constant. The discipline lives on the
		 * pacer thread, so what the door reads is this mirror. */
		CHK(reac_pacer_pace_source(&p) == REAC_PACE_GRAPH_REF);

		/* The transcript names the device, not just the tier. */
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			CHK(f != NULL);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			CHK(strstr(buf, "reac-clock:") != NULL);
			CHK(strstr(buf, "master (pace: generated here)") != NULL);
			CHK(strstr(buf, "acquiring graph clock (RME Babyface Pro)") != NULL);
			CHK(strstr(buf, "locked to graph clock (RME Babyface Pro)") != NULL);
			/* the correction and both periods are on the line, so an operator can
			 * see HOW FAR we are being pulled, not just that we are locked */
			CHK(strstr(buf, " ppm, period ") != NULL);
			CHK(strstr(buf, "vs nominal 125000 ns") != NULL);
			free(buf);
		}

		/* The RME is unplugged mid-run: hold the last good rate, report holdover,
		 * never snap and never keep claiming lock. */
		long held = p.slot_period_ns;
		reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 0, 0, NULL,
		                         REAC_CLOCK_Q_UNGRADED, now);
		run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		CHK(p.clock.state == REAC_CLOCK_HOLDOVER);
		CHK(p.slot_period_ns == held);
		/* HOLDOVER IS NOT A REFERENCE. The period is the last good one and nothing is
		 * steering it now, so the published pace is free-run: naming the device we
		 * stopped following would tell a console we are locked to a box that is out. */
		CHK(reac_pacer_pace_source(&p) == REAC_PACE_FREE_RUN);
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			CHK(strstr(buf, "holdover: reference lost") != NULL);
			free(buf);
		}
	}

	/* A publisher that simply STOPS (crashed, node removed) leaves its presence
	 * bit set. Staleness must still turn that into holdover. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 12000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		long held = p.slot_period_ns;
		/* stop publishing; the bit stays set */
		now += REAC_CLOCK_STALE_NS + 1;
		run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		CHK(atomic_load(&p.clock_present) & REAC_CLOCK_AVAIL_GRAPH);
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);
		CHK(p.clock.state == REAC_CLOCK_HOLDOVER);
		CHK(p.slot_period_ns == held);
	}

	/* ---- 4. BOX: a stagebox that is itself the clock master -------------- *
	 * reac-pw is the REAC MASTER here — it grants and drives the segment — while
	 * being a clock FOLLOWER disciplined to the box's counter slope. First-class,
	 * not a fallback. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -23000,
			                         "S-1608", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_BOX);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(p.slot_period_ns > NOMINAL_NS);        /* slow reference -> longer */
		CHK(p.slot_period_ns == 125003L);          /* 125000 / 0.999977 */
		CHK(reac_pacer_pace_source(&p) == REAC_PACE_BOX_SLOPE);
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			CHK(strstr(buf, "locked to box counter slope (S-1608)") != NULL);
			free(buf);
		}

		/* The RME is then plugged in: a PHC-less rig ranks the graph clock above
		 * the box, so we switch and re-earn the lock. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -23000, "S-1608",
			                         REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 5000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
	}

	/* THE CLOSED-LOOP CASE: a box SLAVED to us recovers its word clock from our
	 * cadence, so its counter slope measured against the host clock is exactly our
	 * own applied correction handed back. The loop's residual is then identically
	 * zero, so it parks where it is and the cadence cannot run away — the same code
	 * that follows a word-clocked box is a harmless no-op against a slaved one.
	 * This is why the box tier is safe to enable unconditionally. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 4000; i++) {
			int echoed = (int)(reac_dll_applied_ppm(&p.clock.dll) * 1000.0);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, echoed, "S-0808",
			                         REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_BOX);
		CHK(p.slot_period_ns == NOMINAL_NS);       /* never walks off nominal */
		CHK(reac_dll_applied_ppm(&p.clock.dll) == 0.0);
	}

	/* ---- 5. the hierarchy, through the wiring ---------------------------- */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, 40000, "S-1608",
			                         REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 20000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 1, 7000, "phc0",
			                         REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_PHC);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(fabs(reac_dll_applied_ppm(&p.clock.dll) - 7.0) < 0.01);
		/* the PHC goes, the graph clock takes over — not the box */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 0, 0, NULL,
			                         REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, 40000, "S-1608",
			                         REAC_CLOCK_Q_UNGRADED, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 20000,
			                         "RME Babyface Pro", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
	}

	/* ---- 6. reference QUALITY through the wiring (#77) -------------------- *
	 * The rig case the issue is actually about: something IS the elected PipeWire
	 * driver, it IS hardware, and it is still the wrong thing to hand a whole REAC
	 * segment's pace to. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);

		/* An HDMI sink is the graph driver and a stagebox is on the segment. The
		 * display clock is present, hardware, and publishing a perfectly plausible
		 * ppm — and it must still lose to the box. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 11000,
			                         "alsa_output.pci-0000_01_00.1.hdmi-stereo",
			                         reac_clock_grade_name(
			                             "alsa_output.pci-0000_01_00.1.hdmi-stereo",
			                             NULL),
			                         now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -23000, "S-1608",
			                         REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_BOX);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			CHK(f != NULL);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			CHK(strstr(buf, "locked to box counter slope (S-1608)") != NULL);
			/* not once, in any state, do we claim the display clock */
			CHK(strstr(buf, "graph clock") == NULL);
			free(buf);
		}
	}

	/* The expected rig configuration, designated by the operator: the transcript
	 * carries the tier next to the device, and the packing of state+quality into
	 * one event byte survives the round trip through the RT ring. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		const char *rme = "alsa_output.usb-RME_Babyface_Pro-00.pro-output-0";
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 18000, rme,
			                         reac_clock_grade_name(rme, "babyface"), now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(reac_clock_disc_quality(&p.clock) == REAC_CLOCK_Q_DESIGNATED);
		CHK(reac_clock_quality_can_own(reac_clock_disc_quality(&p.clock)) == 1);
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			CHK(f != NULL);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			/* the label truncates into the 28-byte event slot; the tier does not */
			CHK(strstr(buf, "locked to graph clock (alsa_output.usb-RME_Ba") != NULL);
			CHK(strstr(buf, "quality: operator-designated") != NULL);
			CHK(strstr(buf, " ppm, period ") != NULL);
			free(buf);
		}
	}

	/* A graph clock that MEASURES badly is flagged, not ejected: it keeps the
	 * segment (ejecting it would reset the measurement, re-admit it, and flap a
	 * live rig on our own opinion) while the transcript says plainly that it is
	 * not fit to own one. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 3000; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1,
			                         (i % 40 == 0) ? 60000 : 20000,
			                         "Some Converter", REAC_CLOCK_Q_UNGRADED, now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);          /* still following it */
		CHK(reac_clock_disc_quality(&p.clock) == REAC_CLOCK_Q_MARGINAL);
		CHK(reac_clock_quality_can_own(reac_clock_disc_quality(&p.clock)) == 0);
		{
			char *buf = NULL;
			size_t len = 0;
			FILE *f = open_memstream(&buf, &len);
			CHK(f != NULL);
			reac_pacer_log_drain(&p, f);
			fclose(f);
			CHK(strstr(buf, "(Some Converter), quality: marginal") != NULL);
			free(buf);
		}
	}

	/* ---- THE GRAPH-CLOCK DOOR ITSELF (2026-09-08). The admission and the grading used
	 * to sit inline in the reac-playback node's RT callback, which made the reference
	 * hostage to somebody having patched audio INTO the box's outputs: an unlinked
	 * playback node is SUSPENDED, its callback never runs, and the daemon reported
	 * "free-running (no reference)" on a rig whose RME was driving everything else and
	 * whose CAPTURE node was linked and running throughout. The decision moved into
	 * reac_pacer_clock_publish_graph so both nodes can take the sample; these are its
	 * arms, which is what a caller can no longer get wrong on its own. */
	{
		struct reac_pacer p;
		uint64_t now = 1000000000ull;
		pacer_clock_init(&p, 1);

		/* A HARDWARE DRIVER IS ADMITTED, graded by name, and followed. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish_graph(&p, "alsa_output.usb-RME_Babyface_Pro",
			                               0, 1.000018, now, NULL);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);

		/* A SOFTWARE TIMER IS NOT A CLOCK. Following PipeWire's dummy driver would be
		 * following our own CLOCK_MONOTONIC through a longer pipe, and would let us
		 * report lock while nothing external disciplines anything. */
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish_graph(&p, "clock.system.monotonic",
			                               0, 1.000018, now, NULL);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);
		CHK(p.slot_period_ns == NOMINAL_NS);

		/* A FREEWHEELING GRAPH IS NOT A CLOCK EITHER, whatever its driver is called. */
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish_graph(&p, "alsa_output.usb-RME_Babyface_Pro",
			                               1, 1.000018, now, NULL);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);

		/* THE OPERATOR'S DESIGNATION RIDES THROUGH the same door and is graded, so the
		 * transcript names the tier the operator asked for rather than a bare guess. */
		pacer_clock_init(&p, 1);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish_graph(&p, "alsa_output.usb-RME_Babyface_Pro",
			                               0, 1.000018, now, "Babyface");
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(reac_clock_disc_quality(&p.clock) == REAC_CLOCK_Q_DESIGNATED ||
		    reac_clock_disc_quality(&p.clock) == REAC_CLOCK_Q_GOOD);

		/* AND IT IS INERT WITH FOLLOWING OFF, like every other publisher here. */
		pacer_clock_init(&p, 0);
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish_graph(&p, "alsa_output.usb-RME_Babyface_Pro",
			                               0, 1.000018, now, NULL);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_FREERUN);
		CHK(p.slot_period_ns == NOMINAL_NS);
	}

	printf("test_reac_pacer_clock: OK\n");
	return 0;
}
