// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock — when a wire nobody configured is knocked on, and when it is left alone.
 *
 * THE DEFECT THIS EXISTS AGAINST, 2026-09-08 22:10 on 0.5.0-2: the S-0808 and the
 * S-1608 were powered, cabled and carrier-up on two NICs, and neither transmitted one
 * 0x8819 frame — rx_packets +0 in five seconds, nothing in eight seconds of capture. A
 * box in slave mode spends a BOUNDED broadcast flood on PHY-up and then goes silent
 * forever if no master answered it, so a daemon that only ever listens can never wake a
 * box that was powered before it started. Two boxes, mute, on a desk with no pins.
 *
 * The safety half is asserted just as hard as the waking half: nothing is EVER emitted
 * before the masterless observation closes, and one frame from anybody ends the knocking
 * for good.
 */
#include "reac_knock.h"
#include "reac_fsm.h"   /* REAC_FSM_FLOOD_BURST — the period's derivation, mechanically */

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

static const uint8_t OURS[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };

int main(void)
{
	struct reac_knock k;
	const uint64_t t0 = 1000ULL * SEC;

	/* ---- A. NOTHING IS TRANSMITTED INSIDE THE MASTERLESS OBSERVATION. This is the
	 * whole safety argument's first half: a wire we have not yet watched for
	 * REAC_KNOCK_LISTEN_NS has not been PROVEN masterless, and a knock onto a wire that
	 * might have a master on it would be exactly the two-masters fault the arbitration
	 * exists to prevent. Stepped at a fine grid so a single early ACT would be caught. */
	reac_knock_init(&k, OURS, t0);
	CHK(k.state == REAC_KNOCK_LISTENING);
	for (uint64_t t = t0; t < t0 + REAC_KNOCK_LISTEN_NS; t += 10 * MS)
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);
	CHK(k.sent == 0);

	/* ---- B. THE OBSERVATION CLOSES ON SILENCE: knock, once, and say so once. */
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_BEGIN);
	CHK(k.state == REAC_KNOCK_KNOCKING);
	CHK(k.sent == 1);
	/* And NOT again on the very next step — a knock is one frame per period, and the
	 * BEGIN line is a one-shot, or the journal carries a line every two seconds forever. */
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS + MS) == REAC_KNOCK_ACT_NONE);

	/* ---- C. ONE PER PERIOD, AND SILENTLY. Run ten periods on a 100 ms poll grid, which
	 * is finer than the period, and count: the poll rate must not change how often the
	 * wire is knocked on. Jitter is bounded by REAC_KNOCK_JITTER_NS, so ten periods land
	 * inside [10P, 10P + 10J] and the count over that span is exactly ten. */
	unsigned long begins = 0, sends = 0;
	uint64_t span = 10 * REAC_KNOCK_PERIOD_NS;
	for (uint64_t t = t0 + REAC_KNOCK_LISTEN_NS; t <= t0 + REAC_KNOCK_LISTEN_NS + span; t += 100 * MS) {
		enum reac_knock_act a = reac_knock_step(&k, t);
		if (a == REAC_KNOCK_ACT_BEGIN) begins++;
		if (a == REAC_KNOCK_ACT_SEND)  sends++;
	}
	CHK(begins == 0);                       /* the BEGIN is spent */
	CHK(sends >= 8 && sends <= 10);         /* ten periods, jitter shortening the tail */

	/* ---- D. THE FIRST FRAME HEARD ENDS IT, AND ENDS IT FOR GOOD. Whatever is on the
	 * wire — a desk's stream, a box's answer, a rival — the hunt classifies it and
	 * decides; nothing more of ours goes out. This is the safety argument's second half:
	 * we never keep announcing over somebody who is already there. */
	reac_knock_heard(&k);
	CHK(k.state == REAC_KNOCK_STOPPED);
	unsigned long sent_at_stop = k.sent;
	CHK(reac_knock_step(&k, t0 + 100 * SEC) == REAC_KNOCK_ACT_END);   /* said once */
	for (uint64_t t = t0 + 101 * SEC; t < t0 + 200 * SEC; t += REAC_KNOCK_PERIOD_NS / 2)
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);
	CHK(k.sent == sent_at_stop);            /* not one frame after the answer */

	/* ---- E. A WIRE THAT ANSWERS INSIDE ITS OBSERVATION SAYS NOTHING AT ALL. The common
	 * case on a working rig: a desk is already streaming when we get carrier. We never
	 * knocked, so there is nothing to report stopping — a "stopped knocking" line about a
	 * knock that never happened is a log that describes a thing that did not occur. */
	reac_knock_init(&k, OURS, t0);
	CHK(reac_knock_step(&k, t0 + 10 * MS) == REAC_KNOCK_ACT_NONE);
	reac_knock_heard(&k);
	CHK(reac_knock_step(&k, t0 + 20 * MS) == REAC_KNOCK_ACT_NONE);
	CHK(reac_knock_step(&k, t0 + 10 * SEC) == REAC_KNOCK_ACT_NONE);
	CHK(k.sent == 0);

	/* ---- F. TWO HOSTS DO NOT LOCK STEP. Same wire, same instant, different MACs: the
	 * jitter has to pull their schedules apart, or two daemons brought up together
	 * announce in the same millisecond forever. Compared over ten periods. */
	struct reac_knock a2, b2;
	static const uint8_t OTHER[6] = { 0x34, 0x5a, 0x60, 0x11, 0x22, 0x33 };
	reac_knock_init(&a2, OURS, t0);
	reac_knock_init(&b2, OTHER, t0);
	int differed = 0;
	for (uint64_t t = t0; t < t0 + 10 * REAC_KNOCK_PERIOD_NS; t += MS) {
		enum reac_knock_act ra = reac_knock_step(&a2, t);
		enum reac_knock_act rb = reac_knock_step(&b2, t);
		if (ra != rb)
			differed = 1;
	}
	CHK(differed);
	/* ...and the jitter stays inside its bound: a2's knocks are still ~one per period. */
	CHK(a2.sent >= 9 && a2.sent <= 11);

	/* ---- G. THE NUMBERS, STATED AS THEIR DERIVATIONS RATHER THAN AS LITERALS, so a
	 * later edit to one end breaks a test instead of quietly breaking the reasoning.
	 *
	 * The period must outlast the box's own bounded presence flood — 5460 frames at the
	 * 48 k box cadence of 4000 fps = 250 µs a frame — or a second knock lands inside the
	 * answer round the first one started. And it is at the operator's 2 s floor. */
	CHK(REAC_KNOCK_PERIOD_NS > (uint64_t)REAC_FSM_FLOOD_BURST * 250000ULL);
	CHK(REAC_KNOCK_PERIOD_NS >= 2 * SEC);
	/* The listening window is 1837 slots at the SLOWEST cadence this daemon serves
	 * (44.1 k / 12 = 3675 fps, 272 µs a slot) — a master fills every one of them. */
	CHK(REAC_KNOCK_LISTEN_NS * 3675ULL / SEC >= 1500);
	/* ...and longer than two of main.c's 200 ms hearing polls, so the silence that
	 * licences a transmission is observed more than once by the loop that acts on it. */
	CHK(REAC_KNOCK_LISTEN_NS > 2 * 200 * MS);
	/* The jitter breaks ties without making the period a fiction. */
	CHK(REAC_KNOCK_JITTER_NS > 0 && REAC_KNOCK_JITTER_NS <= REAC_KNOCK_PERIOD_NS / 4);

	printf("ok: silent for %llu ms of proof, then one announce every %llu s, and the "
	       "first frame heard ends it for good\n",
	       (unsigned long long)(REAC_KNOCK_LISTEN_NS / MS),
	       (unsigned long long)(REAC_KNOCK_PERIOD_NS / SEC));
	return 0;
}
