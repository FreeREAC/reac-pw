// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ENROLL->grant dwell: does ending it early actually DELIVER the grant early?
 *
 * Measured on the live rig 2026-08-30 (S-4000S at 96 kHz, REACPW_GRANT_ON_DECLARE=1,
 * REACPW_GRANT_DWELL_MS=5000): the box declared at +0.85 s and the master still went
 * ESTABLISHED at +5.16 s — the full cap. Instrumenting the early-exit predicate showed
 * it evaluating TRUE from ~tick 401 onward, so the dwell branch WAS being skipped and
 * nothing happened anyway.
 *
 * The reason is the burst timeline, not the predicate. Both the grant-slot index and
 * the completion test are anchored to the grant_dwell CONSTANT:
 *
 *     gt = grant_ticks - 1 - grant_dwell            (negative => no grant slot ever)
 *     grant_delivered = grant_ticks >= grant_dwell + burst_len*stride + 1
 *
 * so skipping the dwell drops us into the burst branch with a negative cursor and the
 * FSM waits out grant_dwell regardless. An early exit that does not re-anchor the burst
 * buys exactly nothing.
 *
 * This drives the rig's own sequence as a pure FSM: rx CONFIG -> GRANTING, then the
 * recognition (reac_master_set_box) lands WHILE granting, which is the window-restart
 * path the S-4000S takes and the S-1608 does not.
 *
 * libreac reads no environment of its own (docs/design/specs/
 * 2026-09-17-tunables-api-and-shared-refusal-codes.md) — this test reads its OWN two env
 * vars (meson runs this binary twice with different env, one process per configuration)
 * and pushes them through reac_master_tunables_set() before touching the FSM, the same
 * doorway the real daemon uses. Both arms matter: the fast one must be fast, and the
 * default must not move.
 */
#include <reac/reac_master.h>
#include <reac/reac_ports.h>
#include <reac/reac_tunables.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define FPS REAC_PKT_RATE_96K                     /* 96 kHz: 8000 downstream frames/s */
#define DWELL_MS 5000                /* the cap both arms are given */
static const uint8_t M_SRC[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };
static const uint8_t B_SRC[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };  /* the rig's S-4000S */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	const char *v = getenv("REACPW_GRANT_ON_DECLARE");
	const int fast = !(v && v[0] == '0');
	const char *dwell_ms_env = getenv("REACPW_GRANT_DWELL_MS");

	struct reac_master_tunables mt = REAC_MASTER_TUNABLES_DEFAULT;
	mt.grant_on_declare = fast;
	if (dwell_ms_env)
		mt.grant_dwell_ms = strtol(dwell_ms_env, NULL, 10);
	reac_master_tunables_set(&mt);

	struct reac_master m;

	reac_master_init(&m, M_SRC, NULL, FPS);

	/* The box's config-announce arrives: PROBING -> GRANTING, exactly as the rig logs
	 * ("rx CONFIG from …08:bc (state GRANTING)"). */
	reac_master_rx(&m, REAC_M_RX_BOX_CONFIG, B_SRC, NULL);

	/* Recognition lands while GRANTING — the window-restart path. 32 in / 8 out is the
	 * S-4000S the rig declared; the head-amp base is the one its announce carries. */
	reac_master_set_box(&m, 32, 8, 0x00);
	CHK(reac_master_has_box(&m));

	const int dwell_slots = (FPS * DWELL_MS) / 1000;
	CHK(m.grant_dwell == dwell_slots);        /* the cap the env asked for */

	/* THE RIG'S SEQUENCE, and the reason a naive harness misses the defect. The box
	 * announces REPEATEDLY — the live log carries two `box JOIN seen` lines 250 us apart
	 * — and reac_pacer calls set_box on every parsed announce. The FIRST call restarts
	 * the grant window (!had_box) and clears enroll_pending; every LATER call has
	 * had_box true, so it re-arms enroll_pending WITHOUT restarting the window. That is
	 * the state the rig probe caught: enroll_pending=1 entering the dwell, so the dwell's
	 * mid-window ENROLL fires at grant_ticks=1 and stamps enroll_sent_tick=1.
	 * Without this second call the early-exit predicate can never arm and the test
	 * measures a path the rig never takes. */
	for (int t = 0; t < 8; t++) {
		uint16_t c0;
		int i0;
		reac_master_next(&m, &c0, &i0);
	}
	reac_master_set_box(&m, 32, 8, 0x00);
	CHK(m.enroll_pending == 1);               /* re-armed, window NOT restarted */

	/* Drive slots until the master reaches ESTABLISHED — the thing the rig measures and
	 * the thing that gates audio. Emitting a grant early is not the same as ARRIVING:
	 * grant_delivered() is what moves the FSM, and it is anchored to grant_dwell. */
	int first_grant = -1, established = -1, enrolls = 0;
	const int limit = dwell_slots * 2;
	for (int t = 0; t < limit && established < 0; t++) {
		uint16_t counter;
		int idx;
		enum reac_master_emit e = reac_master_next(&m, &counter, &idx);
		if (e == REAC_M_EMIT_ENROLL)
			enrolls++;
		else if (e == REAC_M_EMIT_GRANT && first_grant < 0)
			first_grant = t;
		if (m.state == REAC_M_ESTABLISHED)
			established = t;
	}

	CHK(first_grant >= 0);                    /* a grant must happen at all */
	CHK(established >= 0);                    /* and the master must ARRIVE */
	CHK(enrolls >= 1);                        /* the box must be enrolled before it */

	/* The settle is fps/20 = 50 ms; the grant must follow the declaration by about that,
	 * not by the 5 s cap. One announce cadence (fps) is a generous ceiling. */
	if (fast) {
		CHK(first_grant < FPS);
		/* THE ASSERTION THAT MATTERS: arriving, not just emitting. */
		CHK(established < dwell_slots);
		printf("OK: grant-on-declare — first grant slot %d (%.3f s), "
		       "ESTABLISHED slot %d (%.3f s), cap was %.3f s\n",
		       first_grant, (double)first_grant / FPS,
		       established, (double)established / FPS,
		       (double)dwell_slots / FPS);
	} else {
		/* The opt-out must still hold the full dwell — the escape hatch for a box whose
		 * firmware needs the long hold, and the proof the cap itself still works. */
		CHK(first_grant >= dwell_slots);
		CHK(established >= dwell_slots);
		printf("OK: default holds the dwell — first grant slot %d, ESTABLISHED slot %d (%.3f s)\n",
		       first_grant, established, (double)established / FPS);
	}
	return 0;
}
