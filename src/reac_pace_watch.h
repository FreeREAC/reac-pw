/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_pace_watch — is the wire carrying the pace we CLAIM it is?
 *
 * reac-pw picks the REAC pace (`--rate`), paces its TX at it, and publishes its
 * PipeWire node at it. Nothing until now checked whether the box agreed. On the rig,
 * 2026-08-22, it did not: `--rate 96000` gave a byte-perfect 7996 pps downstream (1492-byte
 * frames, 125.1 us mean interval, indistinguishable from a real M-5000) and BOTH boxes kept
 * answering at 4000 pps — 48 kHz. The capture node was published as 96000 Hz over 48 000
 * samples/s, `gaps=0`, PipeWire `ERR 0`. A stream labelled at twice its true rate produces
 * no error anywhere; it just plays an octave low, and every indicator we had read healthy.
 *
 * THE PACE IS THE PACKET RATE. libreac's byte law is pps = rate / 12 in every mode — 44.1k
 * = 3675, 48k = 4000, 96k = 8000, with 12 samples per packet throughout (the 96 kHz mode
 * doubles pps rather than halving channels). So the wire can simply be asked, and
 * `reac_rate_snap()` turns an observed packet rate into the pace it means.
 *
 * This watcher counts frames over a fixed window and says out loud when the answer
 * disagrees with what we configured. It deliberately does NOT change the published rate:
 * a box ignoring the master's pace is an anomaly the operator must SEE, and silently
 * relabelling the node would fix the symptom while hiding that the segment is not doing
 * what the desk told it to.
 */
#ifndef REAC_PACE_WATCH_H
#define REAC_PACE_WATCH_H

#include <stdint.h>

/* One observation window. Long enough that a couple of late frames cannot move the
 * verdict, short enough that a wrong pace is reported within seconds of establishment. */
#define REAC_PACE_WINDOW_NS      2000000000ull

/* A window carrying fewer frames than this is a SEAM, not a pace: a link bounce, a
 * cold-connect or a peer change leaves a short window whose packet rate describes the
 * interruption. Judging it would accuse a box for the shape of its own reconnect. */
#define REAC_PACE_MIN_FRAMES     1000u

/* A window containing an inter-frame gap this large is DISTURBED and gets discarded.
 * The legal paces are close together — 3675 pps (44.1k) is only 8% below 4000 (48k) — so a
 * single stall stretches a window's elapsed time enough to move it across the boundary: a
 * 143 ms hiccup made a healthy 48 kHz segment measure 3738 pps and read as 44.1 kHz. A
 * frame-count floor does not catch that, because the window is full; the stall is in its
 * SHAPE. Nominal spacing is 250 us at 48 kHz, so 5 ms is ~20x anything a real pace produces. */
#define REAC_PACE_MAX_GAP_NS     5000000ull

/* A permanent disagreement is reported on sight, then at most this often. A defect that
 * scrolls the log once per window for the length of a show is one the operator filters out. */
#define REAC_PACE_WARN_INTERVAL_NS 30000000000ull

struct reac_pace_watch {
	int      configured_rate;   /* what --rate asked for */
	int      observed_rate;     /* what the wire last carried; 0 = no verdict yet */
	uint64_t win_start_ns;
	uint64_t win_frames;
	uint64_t win_last_ns;       /* previous frame, to measure inter-frame gaps */
	uint64_t win_max_gap_ns;    /* largest gap in the current window */
	uint64_t warned_ns;         /* 0 = not currently warning */
	uint64_t windows_bad;       /* mismatching windows seen, for the summary */
	uint64_t windows_disturbed; /* windows discarded as seams/stalls */
};

void reac_pace_watch_init(struct reac_pace_watch *w, int configured_rate);

/* Forget the current window and any verdict. Call on a PEER CHANGE: everything learned
 * from the previous peer must go in one place, before it can be mistaken for continuity. */
void reac_pace_watch_reset(struct reac_pace_watch *w, uint64_t now_ns);

/* Count one received frame. Returns 1 when a window has just closed, its pace disagrees
 * with the configured one, AND the rate limit allows reporting — i.e. exactly when the
 * caller should tell the operator. Returns 0 otherwise. */
int reac_pace_watch_frame(struct reac_pace_watch *w, uint64_t now_ns);

/* The last completed window's pace in Hz, or 0 if no window has completed. */
int reac_pace_watch_observed(const struct reac_pace_watch *w);

/* What --rate asked for, in Hz. */
int reac_pace_watch_configured(const struct reac_pace_watch *w);

#endif /* REAC_PACE_WATCH_H */
