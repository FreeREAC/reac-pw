// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tapwait — the hunt waits for the topology tap to place a sighting, and it waits
 * for a BOUNDED time.
 *
 * THE DEFECT THIS EXISTS AGAINST, measured on the operator's desk 2026-09-21 22:17:45
 * (docs/design/notes/2026-09-21-one-stray-frame-pinned-a-wire.md): the wait was two EVER
 * questions, so one misattributed frame suspended role election on `enp128s20f0u6` for the
 * life of the process. That wire is a direct point-to-point cable with one cold S-0808 on
 * the far end; it carried 0 RX packets for the next nine minutes while the daemon reported
 * `listening — role auto`, and eight inputs were off the desk until a human power-cycled
 * the box. The positive control is in the same journal second: `enp131s0` was given the
 * identical sighting, its tap went on classifying real untagged frames, and it was
 * mastering its own S-1608 three seconds later.
 *
 * Both halves are asserted here: the wait that keeps a trunk parent from being driven, and
 * the expiry that keeps a stray frame from pinning a wire.
 */
#include "reac_tapwait.h"

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

int main(void)
{
	const uint64_t t0 = 1000ULL * SEC;
	struct reac_tapwait_in in;

	/* ---- A. THE WAIT ITSELF. A tap is open, it has classified no untagged frame, and
	 * the sniffer has just heard something: the frame's VLAN is unknown, so the hunt
	 * does not elect a role on it this poll. This is the trunk-parent fault — a master
	 * on the parent beside the masters on its sub-interfaces — and it is the reason the
	 * guard exists at all. Stepped on a fine grid across the whole window. */
	for (uint64_t t = t0; t < t0 + REAC_TAPWAIT_NS; t += 10 * MS) {
		in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 0,
		                               .last_heard_ns = t0, .now_ns = t };
		CHK(reac_tapwait_binds(&in) == 1);
	}

	/* ---- B. AND IT EXPIRES. Past the window with no untagged classification and no
	 * further frame, the wire is SILENT, and a silent wire belongs to the masterless
	 * observation exactly as it does on a clean start. This is the assertion the desk
	 * paid nine minutes for. */
	in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 0,
	                               .last_heard_ns = t0, .now_ns = t0 + REAC_TAPWAIT_NS };
	CHK(reac_tapwait_binds(&in) == 0);
	in.now_ns = t0 + 600 * SEC;
	CHK(reac_tapwait_binds(&in) == 0);

	/* ...and a wire that goes on being heard keeps waiting, however long the tap takes.
	 * A real access port re-proves itself thousands of times inside one window, so the
	 * bound can never fire under it; only silence reaches B. */
	for (uint64_t t = t0; t < t0 + 50 * REAC_TAPWAIT_NS; t += REAC_TAPWAIT_NS / 10) {
		in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 0,
		                               .last_heard_ns = t, .now_ns = t };
		CHK(reac_tapwait_binds(&in) == 1);
	}

	/* ---- C. THE TAP HAS SPOKEN: one untagged classification and the wait is over, at
	 * any age of the sighting. The wire is placed on the parent itself and the hunt owns
	 * it — this is the `enp131s0` arm of the live control. */
	in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 1,
	                               .last_heard_ns = t0, .now_ns = t0 + 1 };
	CHK(reac_tapwait_binds(&in) == 0);
	in.untagged = 8000;
	CHK(reac_tapwait_binds(&in) == 0);

	/* ---- D. NO TAP, NOTHING TO WAIT FOR. A VLAN sub-interface has no tap by design
	 * (the kernel already stripped the tag) and a tap that could not open is reported
	 * and the wire served as an ordinary one. Neither may be held up. */
	in = (struct reac_tapwait_in){ .tapped = 0, .untagged = 0,
	                               .last_heard_ns = t0, .now_ns = t0 + 1 };
	CHK(reac_tapwait_binds(&in) == 0);

	/* ---- E. NOTHING EVER HEARD IS NOT A SIGHTING. A quiet wire has nothing for the tap
	 * to place, and holding it here would suspend the very case reac_knock exists for —
	 * the cold box that never speaks first. */
	in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 0,
	                               .last_heard_ns = 0, .now_ns = t0 + 600 * SEC };
	CHK(reac_tapwait_binds(&in) == 0);

	/* ---- F. NO WRAP AT THE BOTTOM OF THE CLOCK. A stamp at or ahead of `now` reads as
	 * just-heard, never as an enormous age — unsigned time subtracts in the right order
	 * or not at all. */
	in = (struct reac_tapwait_in){ .tapped = 1, .untagged = 0,
	                               .last_heard_ns = t0, .now_ns = t0 };
	CHK(reac_tapwait_binds(&in) == 1);
	in.now_ns = t0 - SEC;
	CHK(reac_tapwait_binds(&in) == 1);

	/* ---- G. THE BAR IS THE DAEMON'S EXISTING ONE, stated as its derivation so an edit
	 * to either end breaks a test instead of quietly inventing a second number: the same
	 * span topo_trunk_now believes a trunk verdict for, and the same one the hunt waits
	 * out before deciding a silent wire. */
	CHK(REAC_TAPWAIT_NS == REAC_HUNT_WINDOW_NS);
	/* ...and longer than several of main.c's 200 ms hearing polls, so a tap that is one
	 * or two polls behind on a loaded machine is never overrun. */
	CHK(REAC_TAPWAIT_NS > 4 * 200 * MS);

	printf("ok: an unclassified sighting holds the hunt off a possible trunk parent for "
	       "%llu ms and not one poll longer; the tap speaking ends the wait at any age, "
	       "a wire still being heard waits as long as it takes, and a wire that was "
	       "heard once and went silent is handed back to the masterless observation\n",
	       (unsigned long long)(REAC_TAPWAIT_NS / MS));
	return 0;
}
