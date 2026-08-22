// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_pace_watch — see reac_pace_watch.h for why this exists.

#include "reac_pace_watch.h"

#include <reac/reac.h>   /* reac_rate_snap: pps -> the pace it means */

static void win_open(struct reac_pace_watch *w, uint64_t now_ns)
{
	w->win_start_ns = now_ns;
	w->win_last_ns = now_ns;
	w->win_frames = 0;
	w->win_max_gap_ns = 0;
}

void reac_pace_watch_init(struct reac_pace_watch *w, int configured_rate)
{
	w->configured_rate = configured_rate;
	w->observed_rate = 0;
	w->warned_ns = 0;
	w->windows_bad = 0;
	w->windows_disturbed = 0;
	win_open(w, 0);
}

void reac_pace_watch_reset(struct reac_pace_watch *w, uint64_t now_ns)
{
	w->observed_rate = 0;
	win_open(w, now_ns);
	/* The warning clock deliberately SURVIVES a reset. A box that bounces its link
	 * every few seconds would otherwise re-arm the "report on sight" path each time
	 * and flood exactly the log the rate limit exists to protect. */
}

int reac_pace_watch_frame(struct reac_pace_watch *w, uint64_t now_ns)
{
	if (w->win_start_ns == 0) {         /* first frame ever seen */
		win_open(w, now_ns);
		w->win_frames = 1;
		return 0;
	}

	uint64_t gap = now_ns - w->win_last_ns;
	if (gap > w->win_max_gap_ns)
		w->win_max_gap_ns = gap;
	w->win_last_ns = now_ns;
	w->win_frames++;

	uint64_t elapsed = now_ns - w->win_start_ns;
	if (elapsed < REAC_PACE_WINDOW_NS)
		return 0;

	uint64_t frames = w->win_frames;
	uint64_t max_gap = w->win_max_gap_ns;
	win_open(w, now_ns);

	/* A short window describes an interruption, not a pace. */
	if (frames < REAC_PACE_MIN_FRAMES) {
		w->windows_disturbed++;
		return 0;
	}

	/* A window that STALLED mid-flight is equally not a pace, even though it is full:
	 * the gap stretches its elapsed time and drags the packet rate across a boundary
	 * the legal paces sit very close to. Judge only undisturbed windows. */
	if (max_gap > REAC_PACE_MAX_GAP_NS) {
		w->windows_disturbed++;
		return 0;
	}

	double pps = (double)frames * 1e9 / (double)elapsed;
	int observed = reac_rate_snap(pps);
	w->observed_rate = observed;

	if (observed == w->configured_rate) {
		/* Agreement re-arms the immediate report, so a pace that goes wrong AGAIN
		 * after recovering is not silenced by the previous episode's rate limit. */
		w->warned_ns = 0;
		return 0;
	}

	w->windows_bad++;

	if (w->warned_ns != 0 && now_ns - w->warned_ns < REAC_PACE_WARN_INTERVAL_NS)
		return 0;

	w->warned_ns = now_ns;
	return 1;
}

int reac_pace_watch_observed(const struct reac_pace_watch *w)
{
	return w->observed_rate;
}

int reac_pace_watch_configured(const struct reac_pace_watch *w)
{
	return w->configured_rate;
}
