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
#include "reac_clock.h"

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
		 * loop and one that would be rejected outright. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 150000,
			                         "RME Babyface Pro", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -900000,
			                         "S-1608", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 1, 1000000000,
			                         "phc0", now);
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

		/* The RME appears as the graph driver, running +18 ppm vs the host. */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 18000,
			                         "RME Babyface Pro", now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(p.slot_period_ns < NOMINAL_NS);        /* fast reference -> shorter */
		CHK(p.slot_period_ns == 124998L);          /* 125000 / 1.000018 */

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
		reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 0, 0, NULL, now);
		run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		CHK(p.clock.state == REAC_CLOCK_HOLDOVER);
		CHK(p.slot_period_ns == held);
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
			                         "RME Babyface Pro", now);
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
			                         "S-1608", now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_BOX);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(p.slot_period_ns > NOMINAL_NS);        /* slow reference -> longer */
		CHK(p.slot_period_ns == 125003L);          /* 125000 / 0.999977 */
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
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, -23000, "S-1608", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 5000,
			                         "RME Babyface Pro", now);
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
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, echoed, "S-0808", now);
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
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, 40000, "S-1608", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 20000,
			                         "RME Babyface Pro", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 1, 7000, "phc0", now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_PHC);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
		CHK(fabs(reac_dll_applied_ppm(&p.clock.dll) - 7.0) < 0.01);
		/* the PHC goes, the graph clock takes over — not the box */
		for (int i = 0; i < 200; i++) {
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_PHC, 0, 0, NULL, now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_BOX, 1, 40000, "S-1608", now);
			reac_pacer_clock_publish(&p, REAC_CLOCK_SRC_GRAPH, 1, 20000,
			                         "RME Babyface Pro", now);
			run_slots(&p, REAC_CLOCK_TICK_SLOTS, &now);
		}
		CHK(p.clock.src == REAC_CLOCK_SRC_GRAPH);
		CHK(p.clock.state == REAC_CLOCK_LOCKED);
	}

	printf("test_reac_pacer_clock: OK\n");
	return 0;
}
