// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_node_recover — WHAT TO DO ABOUT A NODE WE BUILT THAT IS NOT THERE.
 *
 * The rig, 2026-09-08: a segment held its reac-playback node, a journal line reading
 * "autodetected S-1608 -> reac-capture 16 in", and no capture node in the graph at all
 * for nine minutes. Every input patch on that box was dead and nothing said so.
 *
 * Detection alone is not the answer, and neither is retrying forever. A retry loop with
 * no bound and no backoff turns one broken segment into a log nobody can read and a
 * rebuild storm on a graph that is already unhappy — the same "counted round-trips"
 * failure as any unbounded loop. So the decision is a small state machine with three
 * answers and it lives HERE, pure, because a decision embedded in a timer callback can
 * only be tested by having the failure.
 *
 * THE LADDER. A freshly connected stream is legitimately not a node yet (PipeWire
 * assigns the id asynchronously), so nothing happens until the node has been missing for
 * a whole grace window. Each rebuild doubles the window, up to a ceiling: a graph that
 * refused us once usually refuses us again immediately, and a graph that is merely busy
 * gets more time on each pass. After the last attempt there is ONE terminal line and
 * then silence — the state is reported, not narrated.
 *
 * A NODE THAT COMES BACK RESETS EVERYTHING, including a give-up: the box may have been
 * re-plugged, the session manager restarted, the segment re-served. Giving up is about
 * this attempt at this node, never a permanent verdict about the segment.
 */
#ifndef REAC_NODE_RECOVER_H
#define REAC_NODE_RECOVER_H

/* Ticks of the caller's own poll (200 ms in main.c) before a missing node is acted on.
 * 2 s is far longer than any healthy connect and far shorter than the nine minutes the
 * defect cost. */
#define REAC_RECOVER_GRACE_TICKS 10

/* How many times the same node is rebuilt before the daemon says so and stops. Five
 * attempts with doubling windows spans ~1 minute, which is long enough to ride out a
 * session manager restart and short enough that the journal still reads like a report. */
#define REAC_RECOVER_MAX_ATTEMPTS 5

/* The ceiling on the doubling, as a shift of the grace window (16x = 32 s). */
#define REAC_RECOVER_MAX_SHIFT 4

enum reac_node_recover_act {
	REAC_RECOVER_WAIT = 0,   /* nothing to do: healthy, inside the window, or given up */
	REAC_RECOVER_REBUILD,    /* destroy and build it again, and say which attempt this is */
	REAC_RECOVER_GIVE_UP,    /* say it ONCE, name the reason, then stay quiet */
};

struct reac_node_recover {
	int absent;     /* consecutive ticks the node has not been on the graph */
	int attempts;   /* rebuilds already tried for this absence */
	int gave_up;    /* the terminal line has been said */
};

void reac_node_recover_init(struct reac_node_recover *r);

/* One poll tick. `on_graph` is the caller's own presence check — for reac-capture,
 * reac_source_node_on_graph, which asks the daemon for a node id rather than trusting
 * that a create call returned. Returns what to do now. */
enum reac_node_recover_act reac_node_recover_step(struct reac_node_recover *r, int on_graph);

/* The window currently being waited out, in ticks. */
int reac_node_recover_window(const struct reac_node_recover *r);

/* The window that was just SPENT, in ticks — what the message after a REBUILD must
 * quote. `window()` has already doubled by then, and quoting it would tell an operator
 * the node had been missing twice as long as it had. */
int reac_node_recover_spent(const struct reac_node_recover *r);

#endif /* REAC_NODE_RECOVER_H */
