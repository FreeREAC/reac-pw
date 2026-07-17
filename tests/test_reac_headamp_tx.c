// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_tx — the MASTER head-amp DMX send scheduler (task #155, item C.7).
 * Pure state + per-frame next(): pins OFF-unless-set, the edge emission on change,
 * and the periodic full-table re-assert (declarative/DMX, so a dropped record can
 * never leave stale box state). No socket, no FSM — the guard that keeps the
 * head-amp overlay from touching establishment is the pacer's FILLER-only stamp,
 * tested separately; here we only prove the scheduler's emission logic. */
#include "reac_headamp_tx.h"
#include "reac_ctrl.h"     /* enum reac_headamp_param */

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	struct reac_headamp_tx t;
	uint8_t ch, p, v;

	/* 1. An empty table is inactive and forever silent. */
	reac_headamp_tx_init(&t, 100);   /* re-assert period = 100*3/2 = 150 frames */
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

	/* 5. Periodic full re-assert (DMX): with no further changes, one sweep of the
	 * 3 SET cells fires once per re-assert period, re-broadcasting ABSOLUTE values.
	 * Run one period + one full sweep worth of frames and collect the sweep. */
	int seen_phantom5 = 0, seen_sens5 = 0, seen_phantom6 = 0, extra = 0, total = 0;
	for (int i = 0; i < 300; i++) {
		if (reac_headamp_tx_next(&t, &ch, &p, &v)) {
			total++;
			if (ch == 5 && p == REAC_HEADAMP_PHANTOM && v == 1) seen_phantom5++;
			else if (ch == 5 && p == REAC_HEADAMP_SENS && v == 0x10) seen_sens5++;
			else if (ch == 6 && p == REAC_HEADAMP_PHANTOM && v == 0) seen_phantom6++;
			else extra++;
		}
	}
	CHK(total == 3);           /* exactly one full sweep of the 3 cells in a period */
	CHK(seen_phantom5 == 1 && seen_sens5 == 1 && seen_phantom6 == 1);
	CHK(extra == 0);           /* only SET cells, absolute values, no phantoms */

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
	reac_headamp_tx_init(&hi, 100);
	CHK(reac_headamp_tx_set(&hi, 0x2f, REAC_HEADAMP_PHANTOM, 1) == 0);  /* S-1608 in 16 */
	CHK(reac_headamp_tx_set(&hi, 0x28, REAC_HEADAMP_SENS, 0x07) == 0);  /* S-1608 in  9 */
	CHK(reac_headamp_tx_set(&hi, REAC_HEADAMP_MAX_CH, REAC_HEADAMP_PHANTOM, 1) == -1);
	/* Both edges reach the wire, in ascending channel order (the dirty scan walks
	 * the flattened table), so 0x28 precedes 0x2f. */
	CHK(reac_headamp_tx_next(&hi, &ch, &p, &v) == 1);
	CHK(ch == 0x28 && p == REAC_HEADAMP_SENS && v == 0x07);
	CHK(reac_headamp_tx_next(&hi, &ch, &p, &v) == 1);
	CHK(ch == 0x2f && p == REAC_HEADAMP_PHANTOM && v == 1);

	printf("OK: head-amp DMX send — off-unless-set, edge-on-change, full re-assert sweep\n");
	return 0;
}
