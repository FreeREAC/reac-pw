// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The enrolment decisions this binary is LINKED AGAINST — a binding test, not a unit test.
 *
 * The masterless observation (reac_knock) and the tap-wait (reac_tapwait) are libreac's
 * since 1.4.0: deciding what a wire is belongs to the library, and reac-pw deals with
 * enrolled nodes (operator ruling 2026-09-22, libreac
 * docs/design/specs/2026-09-22-enrolment-decisions-belong-to-the-library.md). Their
 * arithmetic is asserted THERE, in libreac's own `make test`, where a libreac change can
 * see it go red — which is exactly what it could not do while those files lived here.
 *
 * WHAT IS LEFT FOR THIS REPO TO ASK, and libreac cannot ask for us: does the library at
 * the other end of this build's `dependency('libreac')` still carry the two rules main.c's
 * 200 ms poll leans on? A version floor proves a NUMBER. This proves the BEHAVIOUR, by
 * calling the installed surface with no reac-pw source and no local header in the build.
 *
 * THE TWO RULES, and the nine minutes they cost on 2026-09-21 22:17:45 (the journal is in
 * libreac's spec §0 and in reac-pw docs/design/notes/2026-09-21-one-stray-frame-pinned-a-
 * wire.md): a frame belonging to a box on ANOTHER interface was misattributed to
 * `enp128s20f0u6`'s sniffer in the instant it opened. One frame cancelled the licence for
 * the life of the process and one frame made an EVER-true "heard anything" bind the hunt
 * forever; the wire then carried 0 RX packets for nine minutes with a cold S-0808 on the
 * far end and eight inputs off the desk. Both latches are gone, and a library that brings
 * either one back must fail at this gate rather than on a rig.
 *
 * It also carries its own positive controls — the silent wire that IS licensed and the
 * fresh sighting that DOES bind — so "no latch" can never be satisfied by a library whose
 * functions simply do nothing.
 */
#include <reac/reac_knock.h>
#include <reac/reac_tapwait.h>
#include <reac/reac_hunt.h>   /* REAC_HUNT_WINDOW_NS — the bar the tap-wait must still use */
#include <reac/reac.h>        /* LIBREAC_VERSION_AT_LEAST — the floor, stated in code */

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

#if !LIBREAC_VERSION_AT_LEAST(1, 4, 0)
#error "reac-pw 1.0.23 needs libreac >= 1.4.0: reac_knock.h and reac_tapwait.h live there now"
#endif

int main(void)
{
	const uint64_t t0 = 1000ULL * SEC;

	/* ---- 1. THE LINKED LIBRARY STILL GRANTS THE LICENCE AT ALL. The positive control
	 * for everything below: a wire that carries nothing for the observation window is
	 * driven. Without this arm, a library whose reac_knock_step never answers DRIVE
	 * would satisfy every "no latch" assertion vacuously. */
	struct reac_knock k;
	reac_knock_init(&k, t0);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS / 2) == REAC_KNOCK_ACT_NONE);
	CHK(reac_knock_step(&k, t0 + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_DRIVE);

	/* ---- 2. AND A CANCELLED OBSERVATION RE-OPENS. The rule the desk paid for. One
	 * stray frame must cost one whole observation measured from that frame — never the
	 * life of the process. A library with the old terminal REAC_KNOCK_CANCELLED reds
	 * here, which is the point of this file. */
	reac_knock_init(&k, t0);
	const uint64_t stray = t0 + REAC_KNOCK_LISTEN_NS / 3;
	reac_knock_heard(&k, stray);
	CHK(reac_knock_step(&k, stray + REAC_KNOCK_LISTEN_NS - 1) == REAC_KNOCK_ACT_NONE);
	CHK(reac_knock_step(&k, stray + REAC_KNOCK_LISTEN_NS) == REAC_KNOCK_ACT_DRIVE);

	/* ---- 3. A WIRE THAT GOES ON BEING HEARD IS NEVER DRIVEN. The safety half, which
	 * the re-open must not have weakened: a real master fills every audio slot, so it
	 * re-cancels thousands of times inside one window. */
	reac_knock_init(&k, t0);
	for (uint64_t t = t0; t < t0 + 20 * REAC_KNOCK_LISTEN_NS;
	     t += REAC_KNOCK_LISTEN_NS / 10) {
		reac_knock_heard(&k, t);
		CHK(reac_knock_step(&k, t) == REAC_KNOCK_ACT_NONE);
	}
	CHK(k.granted == 0);

	/* ---- 4. THE TAP-WAIT STILL BINDS A FRESH SIGHTING — the other positive control,
	 * and the guard that keeps main.c from electing a role on a possible trunk parent
	 * before the topology tap has placed the frame. */
	struct reac_tapwait_in in = { .tapped = 1, .untagged = 0,
	                              .last_heard_ns = t0, .now_ns = t0 + 10 * MS };
	CHK(reac_tapwait_binds(&in) == 1);

	/* ---- 5. AND IT EXPIRES. The second latch of 2026-09-21: "has anything been heard
	 * here, EVER" stayed 1 on one misattributed frame and skipped the segment for the
	 * life of the process. Past the window with no untagged classification and no
	 * further frame, the wire is silent and goes back to the masterless observation. */
	in.now_ns = t0 + REAC_TAPWAIT_NS;
	CHK(reac_tapwait_binds(&in) == 0);
	in.now_ns = t0 + 600 * SEC;
	CHK(reac_tapwait_binds(&in) == 0);

	/* ---- 6. THE TWO BARS ARE STILL ONE NUMBER. main.c reads the tap-wait's window and
	 * the hunt's window as the same span (#102: the hunt asks about the wire as it is
	 * NOW); a library that split them into two constants would change how long this
	 * daemon holds a wire without changing one line here. */
	CHK(REAC_TAPWAIT_NS == REAC_HUNT_WINDOW_NS);

	printf("ok: the linked libreac %s still decides the wire the way this daemon's poll "
	       "assumes — a silent wire is driven after %llu ms, a stray frame costs one whole "
	       "observation and not the process, a wire still being heard is never driven, and "
	       "an unplaced sighting binds the hunt for %llu ms and not one poll longer\n",
	       reac_version(),
	       (unsigned long long)(REAC_KNOCK_LISTEN_NS / MS),
	       (unsigned long long)(REAC_TAPWAIT_NS / MS));
	return 0;
}
