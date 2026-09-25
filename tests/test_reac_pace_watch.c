// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_pace_watch — DOES THE WIRE CARRY THE PACE WE CLAIM IT DOES?
 *
 * Measured on the rig 2026-08-22: with `--rate 96000` our master paced its TX at
 * 7996 pps with byte-identical frames to a real 96 kHz desk, and BOTH boxes kept
 * answering at 4000 pps — 48 kHz. reac-pw published its capture node as
 * `F32P 16 96000` over 48 000 samples/s anyway, reported `gaps=0`, and PipeWire
 * reported `ERR 0`. Every soft indicator read healthy over audio labelled at twice
 * the rate it carried.
 *
 * The pace IS the packet rate (libreac's byte law: pps = rate / 12), so the wire
 * can be asked directly. This watcher does that and says so out loud.
 */
#include <reac/transport/reac_pace_watch.h>

#include <stdio.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define SEC 1000000000ull

/* Feed `pps` frames spread across one second of window, returning how many times
 * the watcher reported a mismatch worth telling the operator about. */
static int run_seconds(struct reac_pace_watch *w, uint64_t *now, int pps, int seconds)
{
	int reports = 0;
	for (int s = 0; s < seconds; s++) {
		for (int i = 0; i < pps; i++) {
			*now += SEC / (uint64_t)pps;
			if (reac_pace_watch_frame(w, *now))
				reports++;
		}
	}
	return reports;
}

int main(void)
{
	uint64_t now = 1000ull * SEC;   /* an arbitrary monotonic origin */

	/* 1. THE AGREEING CASE IS SILENT. A 48 kHz master seeing 4000 pps has
	 * nothing to report, however long it runs — an alarm that cries on the
	 * healthy rig is an alarm the operator learns to ignore. */
	struct reac_pace_watch ok;
	reac_pace_watch_init(&ok, REAC_SAMPLE_RATE_48K);
	CHK(run_seconds(&ok, &now, REAC_PKT_RATE_48K, 30) == 0);
	CHK(reac_pace_watch_observed(&ok) == REAC_SAMPLE_RATE_48K);

	/* 2. THE RIG'S ACTUAL DEFECT: configured 96 kHz, wire carrying 4000 pps.
	 * It must report, and it must report the FIRST time rather than after a
	 * settling period — the audio is already wrong. */
	struct reac_pace_watch bad;
	reac_pace_watch_init(&bad, REAC_SAMPLE_RATE_96K);
	int first = run_seconds(&bad, &now, REAC_PKT_RATE_48K, 3);
	CHK(first >= 1);
	CHK(reac_pace_watch_observed(&bad) == REAC_SAMPLE_RATE_48K);
	CHK(reac_pace_watch_configured(&bad) == REAC_SAMPLE_RATE_96K);

	/* 3. IT IS RATE-LIMITED. A permanent disagreement must not flood the log
	 * once per window for the length of a show. */
	int later = run_seconds(&bad, &now, REAC_PKT_RATE_48K, 120);
	CHK(later >= 1);            /* it keeps saying so ... */
	CHK(later <= 8);            /* ... but not once per 2 s window (60 windows) */

	/* 4. A BOX THAT FOLLOWS CLEARS IT. Once the wire agrees, the watcher goes
	 * quiet and its observation tracks the wire, so a recovered segment is not
	 * left permanently accused. */
	int recovered = run_seconds(&bad, &now, REAC_PKT_RATE_96K, 20);
	CHK(reac_pace_watch_observed(&bad) == REAC_SAMPLE_RATE_96K);
	CHK(recovered == 0);

	/* 5. A PEER CHANGE RESETS THE WINDOW. Establishment and reconnect seams
	 * are partial windows; counting them as a pace would accuse a box for the
	 * shape of its own cold-connect. */
	struct reac_pace_watch seam;
	reac_pace_watch_init(&seam, REAC_SAMPLE_RATE_48K);
	for (int i = 0; i < 500; i++) {         /* a partial window ... */
		now += SEC / REAC_PKT_RATE_48K;
		reac_pace_watch_frame(&seam, now);
	}
	reac_pace_watch_reset(&seam, now);      /* ... then the peer changes */
	CHK(reac_pace_watch_observed(&seam) == 0);   /* no verdict from a seam */
	CHK(run_seconds(&seam, &now, REAC_PKT_RATE_48K, 10) == 0);

	/* 5b. A STALLED WIRE IS A SEAM, NOT A PACE. A window that closes carrying a
	 * handful of frames — a link stall, a box mid-reconnect — has a packet rate
	 * near zero, which snaps to the lowest legal pace and would accuse the box of
	 * running 44.1 kHz. The window must be discarded instead. */
	struct reac_pace_watch stall;
	reac_pace_watch_init(&stall, REAC_SAMPLE_RATE_48K);
	CHK(run_seconds(&stall, &now, REAC_PKT_RATE_48K, 6) == 0);      /* healthy, agreeing */
	CHK(reac_pace_watch_observed(&stall) == REAC_SAMPLE_RATE_48K);
	for (int i = 0; i < 20; i++) {                     /* 20 frames over ~3 s */
		now += SEC / 7;
		CHK(reac_pace_watch_frame(&stall, now) == 0);  /* never reports a stall */
	}
	CHK(reac_pace_watch_observed(&stall) == REAC_SAMPLE_RATE_48K);    /* verdict UNCHANGED */

	/* 5c. A THIN WIRE IS NOT A PACE EITHER. A window can be smoothly paced and still
	 * carry far too few frames to be REAC — 900 frames across 2 s is 450 pps with
	 * 2.2 ms spacing, under the stall threshold, so the gap guard does not catch it.
	 * Its packet rate snaps to the lowest legal pace and would accuse the box of
	 * 44.1 kHz. The frame floor is what refuses it. */
	struct reac_pace_watch thin;
	reac_pace_watch_init(&thin, REAC_SAMPLE_RATE_48K);
	CHK(run_seconds(&thin, &now, REAC_PKT_RATE_48K, 6) == 0);
	CHK(reac_pace_watch_observed(&thin) == REAC_SAMPLE_RATE_48K);
	for (int i = 0; i < 1800; i++) {                   /* 450 pps for ~4 s */
		now += SEC / 450;
		CHK(reac_pace_watch_frame(&thin, now) == 0);
	}
	CHK(reac_pace_watch_observed(&thin) == REAC_SAMPLE_RATE_48K);     /* verdict UNCHANGED */

	/* 6. 44.1 kHz IS A LEGAL PACE and must not read as a 48 kHz fault: 3675 pps
	 * is only 8% from 4000, so a sloppy comparison would call it a mismatch. */
	struct reac_pace_watch f44;
	reac_pace_watch_init(&f44, REAC_SAMPLE_RATE_44K1);
	CHK(run_seconds(&f44, &now, REAC_PKT_RATE_44K1, 20) == 0);
	CHK(reac_pace_watch_observed(&f44) == REAC_SAMPLE_RATE_44K1);

	/* 7. A SILENT WIRE IS NOT A PACE. No frames means no verdict — an unclaimed
	 * REAC box transmits nothing, so zero is "nobody is talking", never 44.1k. */
	struct reac_pace_watch quiet;
	reac_pace_watch_init(&quiet, REAC_SAMPLE_RATE_48K);
	CHK(reac_pace_watch_observed(&quiet) == 0);

	printf("test_reac_pace_watch: OK\n");
	return 0;
}
