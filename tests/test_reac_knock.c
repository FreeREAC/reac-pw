// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock — when a wire nobody configured may be DRIVEN, and when it must be left
 * alone. The licence, not a transmission: this module never puts a frame anywhere.
 *
 * THE DEFECT THIS EXISTS AGAINST, 2026-09-08 22:10 on 0.5.0-2: the S-0808 and the
 * S-1608 were powered, cabled and carrier-up, and neither transmitted one 0x8819 frame —
 * rx_packets +0 in five seconds, nothing in eight seconds of capture. A box in slave mode
 * spends a BOUNDED broadcast flood on PHY-up and then goes silent forever if no master
 * answered it, so a daemon that only ever listens can never wake a box that was powered
 * before it started. Two boxes, mute, on a desk with no pins.
 *
 * AND THE CORRECTION, on the rig with 0.5.0-3: the first cut answered with ONE master
 * announce every two seconds, and the box never answered — tx +2 per ~6 s, rx +0 for over
 * a minute. A cold box answers a master that is DRIVING, so this gate now licences taking
 * the wire rather than knocking on it.
 *
 * The safety half is asserted as hard as the waking half: nothing is licensed before the
 * masterless observation closes, and one frame from anybody cancels it.
 */
#include "reac_knock.h"
#include <reac/reac_fsm.h>   /* REAC_FSM_FLOOD_BURST — the cold box's own bounded flood */

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

int main(void)
{
	struct reac_knock k;
	const uint64_t t0 = 1000ULL * SEC;

	/* ---- A. NOTHING IS LICENSED INSIDE THE MASTERLESS OBSERVATION. The whole safety
	 * argument's first half: a wire we have not yet watched for REAC_KNOCK_LISTEN_NS has
	 * not been PROVEN masterless, and driving one that might have a master on it is
	 * exactly the two-masters fault the arbitration exists to prevent. Stepped on a fine
	 * grid so a single early licence would be caught. */
	reac_knock_init(&k, t0);
	CHK(k.state == REAC_KNOCK_LISTENING);
	for (uint64_t t = t0; t < t0 + REAC_KNOCK_LISTEN_NS; t += 10 * MS)
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);
	CHK(k.granted == 0);

	/* ---- B. THE OBSERVATION CLOSES ON SILENCE: drive, and say so ONCE. The licence is a
	 * one-shot — the segment it starts outlives this object, and a second DRIVE would be
	 * a second serve of a wire already served. */
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_DRIVE);
	CHK(k.state == REAC_KNOCK_PROVEN);
	CHK(k.granted == 1);
	for (uint64_t t = t0 + REAC_KNOCK_LISTEN_NS; t < t0 + 60 * SEC; t += 100 * MS)
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);

	/* ---- C. ONE FRAME FROM ANYBODY CANCELS IT, and cancels it for good. A wire with
	 * something on it was never the case this gate is for: the ordinary hunt joins a
	 * desk, refuses a stagebox on M, and waits out its own window for a box. */
	reac_knock_init(&k, t0);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS / 2) == REAC_KNOCK_ACT_NONE);
	reac_knock_heard(&k);
	CHK(k.state == REAC_KNOCK_CANCELLED);
	for (uint64_t t = t0; t < t0 + 300 * SEC; t += SEC)
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);
	CHK(k.granted == 0);

	/* ...even one frame in the very last instant of the window. The boundary is where a
	 * gate like this fails, and "heard at all" is the bar, not "heard early". */
	reac_knock_init(&k, t0);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS - 1) == REAC_KNOCK_ACT_NONE);
	reac_knock_heard(&k);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_NONE);
	CHK(k.granted == 0);

	/* ---- D. A FRAME AFTER THE LICENCE DOES NOT REVOKE IT HERE. Once the wire is taken
	 * the segment exists, and giving it up is a drop-and-re-serve of two engines, a
	 * socket and a segment lock — main.c's hearing_yield over the retained sniffer, not
	 * a pure clock's business. This asserts the boundary of what this module claims. */
	reac_knock_init(&k, t0);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_DRIVE);
	reac_knock_heard(&k);
	CHK(k.state == REAC_KNOCK_PROVEN);

	/* ---- E. THE WINDOW, STATED AS ITS DERIVATION rather than as a literal, so a later
	 * edit to either end breaks a test instead of quietly breaking the reasoning.
	 *
	 * A master fills every audio slot: sampleRate/12 frames a second, so the SLOWEST rate
	 * on the closed list, 44.1 k, is 3675 fps = 272 us a slot. The window must be exactly
	 * the 1837 consecutive slots the header claims a present master would have had to
	 * transmit into and did not. */
	CHK(REAC_KNOCK_LISTEN_NS * 3675ULL / SEC == 1837);
	/* ...and longer than two of main.c's 200 ms hearing polls, so the silence that
	 * licences driving is observed more than once by the loop that acts on it. */
	CHK(REAC_KNOCK_LISTEN_NS > 2 * 200 * MS);
	/* It is also far SHORTER than the box's own bounded presence flood (5460 frames at
	 * the 48 k box cadence of 4000 fps = 250 us a frame, ~1.36 s), which is the point: a
	 * box that IS flooding is heard inside the window and cancels the licence, so the
	 * licence is only ever granted to a wire whose box has already gone quiet — the case
	 * that had no answer at all before. */
	CHK(REAC_KNOCK_LISTEN_NS < (uint64_t)REAC_FSM_FLOOD_BURST * 250000ULL);

	printf("ok: silent for %llu ms of proof and then the wire may be DRIVEN, one frame "
	       "from anybody cancels it, and nothing is licensed before the window closes\n",
	       (unsigned long long)(REAC_KNOCK_LISTEN_NS / MS));
	return 0;
}
