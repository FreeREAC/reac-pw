// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_tx — the MASTER head-amp send scheduler (task #155, item C.7).
 * Pure state + per-frame next(): pins OFF-unless-set, the edge emission on
 * change, and the one-shot complete-scene REPLAY armed at every establishment —
 * the mechanism that restores a power-cycled box's pins (task #179; measured:
 * m200-s1608-BIDIR-reboot, the M-200 replays the full width x 3 scene 10 ms
 * behind every grant, then goes silent). There is NO periodic re-assert: the
 * committed captures hold 20.8-33 s of established traffic with phantom lit and
 * zero head-amp records. No socket, no FSM — the guard that keeps the head-amp
 * overlay from touching establishment is the pacer's FILLER-only stamp, tested
 * separately; here we only prove the scheduler's emission logic. */
#include "reac_headamp_tx.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param */

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	struct reac_headamp_tx t;
	uint8_t ch, p, v;

	/* 1. An empty table with no replay armed is inactive and forever silent. */
	reac_headamp_tx_init(&t);
	CHK(t.active == 0);
	for (int i = 0; i < 500; i++)
		CHK(reac_headamp_tx_next(&t, &ch, &p, &v) == 0);

	/* 2. Setting a cell arms the sender and emits the CHANGE on the next slot. */
	CHK(reac_headamp_tx_set(&t, 5, REAC_HEADAMP_PHANTOM, 1) == 0);
	CHK(t.active == 1);
	CHK(reac_headamp_tx_next(&t, &ch, &p, &v) == 1);
	CHK(ch == 5 && p == REAC_HEADAMP_PHANTOM && v == 1);
	CHK(reac_headamp_tx_next(&t, &ch, &p, &v) == 0);   /* edge consumed */

	/* 3. Bad args are rejected and change nothing. The channel bound is the
	 * head-amp WIRE-channel space (0x00..0x2f) — see step 7. */
	CHK(reac_headamp_tx_set(&t, REAC_HEADAMP_MAX_CH, REAC_HEADAMP_PHANTOM, 0) == -1);
	CHK(reac_headamp_tx_set(&t, 0, 0x03, 0) == -1);                 /* bad param */
	CHK(reac_headamp_tx_set(&t, 0, REAC_HEADAMP_PHANTOM, 2) == -1); /* bad bool  */
	CHK(reac_headamp_tx_set(&t, 0, REAC_HEADAMP_SENS, 0x38) == -1); /* bad sens  */

	/* 4. Two more cells: each change is emitted (edge), one record per call. */
	CHK(reac_headamp_tx_set(&t, 5, REAC_HEADAMP_SENS, 0x10) == 0);
	CHK(reac_headamp_tx_set(&t, 6, REAC_HEADAMP_PHANTOM, 0) == 0);
	int edges = 0;
	for (int i = 0; i < 4 && edges < 3; i++)
		if (reac_headamp_tx_next(&t, &ch, &p, &v))
			edges++;
	CHK(edges == 2);   /* exactly the two new changes */

	/* 5. NO periodic re-assert. The committed captures show a real M-200 keeps
	 * head-amp silence once the box is armed — 20.8 s / 27.8 s / 33.0 s of
	 * established traffic with phantom lit and zero head-amp records
	 * (COLDCONNECT-clean-2026-07-24, BIDIR-reboot-2026-07-11, matrix-m200-s1608).
	 * The box HOLDS its committed state while powered; the answer to a
	 * power-cycle is the scene replay (step 8), never a timer. Run a long
	 * stretch with no further changes and confirm nothing emits. */
	int total = 0;
	for (int i = 0; i < 1000; i++)
		if (reac_headamp_tx_next(&t, &ch, &p, &v))
			total++;
	CHK(total == 0);           /* committed state is held by the box, never re-pushed */

	/* 6. A re-set to the SAME value still re-arms the edge (an operator re-press is
	 * honoured), proving the change path does not depend on a value delta. */
	CHK(reac_headamp_tx_set(&t, 6, REAC_HEADAMP_PHANTOM, 0) == 0);
	CHK(reac_headamp_tx_next(&t, &ch, &p, &v) == 1);
	CHK(ch == 6 && p == REAC_HEADAMP_PHANTOM && v == 0);

	/* 7. The channel space is the head-amp WIRE-channel range 0x00..0x2f, NOT
	 * libreac's REAC_MAX_CHANNELS (40 AUDIO slots). Conflating the two was a real
	 * bug: a 16-input S-1608 is allocated at base 0x20 and so owns wire channels
	 * 0x20..0x2f = 32..47, meaning its inputs 9..16 were SILENTLY REJECTED here and
	 * could never be given phantom/pad/sens. A fresh table, so the sweep counts
	 * above are untouched. */
	struct reac_headamp_tx hi;
	reac_headamp_tx_init(&hi);
	CHK(reac_headamp_tx_set(&hi, 0x2f, REAC_HEADAMP_PHANTOM, 1) == 0);  /* S-1608 in 16 */
	CHK(reac_headamp_tx_set(&hi, 0x28, REAC_HEADAMP_SENS, 0x07) == 0);  /* S-1608 in  9 */
	CHK(reac_headamp_tx_set(&hi, REAC_HEADAMP_MAX_CH, REAC_HEADAMP_PHANTOM, 1) == -1);
	/* Both edges reach the wire, in ascending channel order (the dirty scan walks
	 * the flattened table), so 0x28 precedes 0x2f. */
	CHK(reac_headamp_tx_next(&hi, &ch, &p, &v) == 1);
	CHK(ch == 0x28 && p == REAC_HEADAMP_SENS && v == 0x07);
	CHK(reac_headamp_tx_next(&hi, &ch, &p, &v) == 1);
	CHK(ch == 0x2f && p == REAC_HEADAMP_PHANTOM && v == 1);

	/* 8. THE POWER-CYCLE RESTORE (task #179): arming the scene on an ALL-UNSET
	 * table emits the complete width x 3 scene — enrolling defaults everywhere
	 * (phantom OFF, pad OFF, SENS non-zero), per-channel param triples in order,
	 * one record per REAC_HEADAMP_SWEEP_STRIDE frames. An S-0808 at base 0. */
	struct reac_headamp_tx rp;
	reac_headamp_tx_init(&rp);
	reac_headamp_tx_arm_scene(&rp, 0x00, 8);
	int records = 0, calls = 0;
	uint8_t last_ch = 0xff, last_p = 0xff;
	while (records < 8 * REAC_HEADAMP_NPARAMS && calls < 8 * 3 * REAC_HEADAMP_SWEEP_STRIDE + 8) {
		calls++;
		if (!reac_headamp_tx_next(&rp, &ch, &p, &v))
			continue;
		/* order: ch ascending from base, params 0,1,2 within each channel */
		int idx = records;
		CHK(ch == (uint8_t)(idx / REAC_HEADAMP_NPARAMS));
		CHK(p == (uint8_t)(idx % REAC_HEADAMP_NPARAMS));
		CHK(v == reac_headamp_default(p));
		last_ch = ch; last_p = p;
		records++;
	}
	CHK(records == 8 * REAC_HEADAMP_NPARAMS);
	CHK(last_ch == 7 && last_p == REAC_HEADAMP_SENS);
	CHK(reac_headamp_default(REAC_HEADAMP_PHANTOM) == 0x00);  /* never 48V by default */
	CHK(reac_headamp_default(REAC_HEADAMP_SENS) != 0x00);     /* non-zero = enrols */
	/* the stride really spreads the burst: strictly more calls than records */
	CHK(calls >= records * REAC_HEADAMP_SWEEP_STRIDE - (REAC_HEADAMP_SWEEP_STRIDE - 1));
	/* scene complete -> silence again (no timer brings it back) */
	for (int i = 0; i < 1000; i++)
		CHK(reac_headamp_tx_next(&rp, &ch, &p, &v) == 0);

	/* 9. The replay carries the OPERATOR's values where set — the show's 48V comes
	 * back after an outage, not the default. A deliberate 0 counts as set. */
	CHK(reac_headamp_tx_set(&rp, 2, REAC_HEADAMP_PHANTOM, 1) == 0);
	CHK(reac_headamp_tx_set(&rp, 3, REAC_HEADAMP_SENS, 0x00) == 0);  /* deliberate 0 */
	while (reac_headamp_tx_next(&rp, &ch, &p, &v)) {} /* drain the two edges */
	reac_headamp_tx_arm_scene(&rp, 0x00, 8);
	int seen_ph2 = -1, seen_s3 = -1;
	records = 0;
	for (int i = 0; i < 8 * 3 * REAC_HEADAMP_SWEEP_STRIDE + 8 && records < 24; i++) {
		if (!reac_headamp_tx_next(&rp, &ch, &p, &v))
			continue;
		records++;
		if (ch == 2 && p == REAC_HEADAMP_PHANTOM) seen_ph2 = v;
		if (ch == 3 && p == REAC_HEADAMP_SENS)    seen_s3 = v;
	}
	CHK(records == 24);
	CHK(seen_ph2 == 1);    /* the operator's 48V rides the replay */
	CHK(seen_s3 == 0x00);  /* the deliberate 0 rides it too — set, not default */

	/* 10. An operator EDGE preempts a replay in progress (phantom-off must not
	 * wait out the burst), and the replay still completes afterwards. */
	reac_headamp_tx_arm_scene(&rp, 0x00, 8);
	CHK(reac_headamp_tx_next(&rp, &ch, &p, &v) == 1);   /* replay record 1 */
	CHK(reac_headamp_tx_set(&rp, 7, REAC_HEADAMP_PHANTOM, 0) == 0);
	CHK(reac_headamp_tx_next(&rp, &ch, &p, &v) == 1);   /* the edge, immediately */
	CHK(ch == 7 && p == REAC_HEADAMP_PHANTOM && v == 0);
	records = 1;
	for (int i = 0; i < 8 * 3 * REAC_HEADAMP_SWEEP_STRIDE + 8 && records < 24; i++)
		if (reac_headamp_tx_next(&rp, &ch, &p, &v))
			records++;
	CHK(records == 24);   /* the full scene still went out */

	/* 11. Arming clamps to the table: base+width past REAC_HEADAMP_MAX_CH replays
	 * only the slots that exist; width 0 arms nothing. */
	struct reac_headamp_tx cl;
	reac_headamp_tx_init(&cl);
	reac_headamp_tx_arm_scene(&cl, REAC_HEADAMP_MAX_CH - 2, 16);
	records = 0;
	for (int i = 0; i < 16 * 3 * REAC_HEADAMP_SWEEP_STRIDE + 16; i++)
		if (reac_headamp_tx_next(&cl, &ch, &p, &v)) {
			CHK(ch < REAC_HEADAMP_MAX_CH);
			records++;
		}
	CHK(records == 2 * REAC_HEADAMP_NPARAMS);
	reac_headamp_tx_arm_scene(&cl, 0, 0);
	CHK(reac_headamp_tx_next(&cl, &ch, &p, &v) == 0);

	printf("OK: head-amp send — off-unless-set, edge-on-change, scene replay at establish, no periodic re-assert\n");
	return 0;
}
