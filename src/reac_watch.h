// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_watch — A SEGMENT THAT IS UP CAN STILL BE RE-DECIDED, and this says how.
 *
 * The hunt (reac_hunt.h) elects a role on a wire nobody configured, and until 0.5.4 it
 * mostly died the moment its segment was served: the wire was classified once, the
 * answer was kept, and anything that turned up afterwards was published as a conflict
 * nobody could act on. That is wrong in one direction and useless in the other. The
 * venue case is the whole argument: a house console hears the stageboxes on a wire
 * nobody pinned, grants them and drives — and then the Roland desk is switched on, and
 * later switched off again. Two masters on one segment is the fault the seglock exists
 * to make impossible between our own processes, and it is no better against a real desk;
 * a segment left slaved to a desk that has gone home is a dead segment with the
 * console's own boxes on it.
 *
 * So the sniffer is KEPT on every segment whose role is still ours to change, and this
 * module is what a fresh verdict off that sniffer MEANS. Two calls, both pure:
 * `reac_watch_keep` at the serve, `reac_watch_decide` on every verdict change after it.
 *
 * IT IS THE PIN THAT DECIDES, NOT THE EVIDENCE WE WON THE WIRE WITH (operator ruling,
 * 2026-09-09; DESIGN.md 0.5.4). 0.5.0 kept the sniffer only where the wire had been taken
 * on PROVEN SILENCE, on the argument that driving it was a bet — and a wire won because a
 * box was heard on it was "evidence" and kept nothing. The distinction does not survive
 * the venue: both wires are unpinned, both are ours only until somebody who owns a clock
 * turns up. What the operator wrote down in `REAC_ROLE_<segment>` is the one answer this
 * module never overturns.
 *
 * WHY IT IS PURE AND NOT THREE `if`s IN THE POLL, WHERE IT USED TO LIVE. The only thing
 * that could exercise it there was a 70-second whole-binary veth run, and that run cannot
 * choose which route took the wire (a stagebox's cold-connect flood is FILLER, which the
 * discovery peer lock deliberately refuses to treat as a sighting, so a box that has not
 * joined yet leaves the wire looking empty). The case this release is about was
 * untestable by construction. Here it is a table, with the inputs written down.
 *
 * Main-thread only, like everything that reads a hunt. Decides; acts on nothing.
 */
#ifndef REAC_WATCH_H
#define REAC_WATCH_H

#include "reac_hunt.h"

#include <stdint.h>

/** What a fresh verdict on a segment that is ALREADY SERVED asks the caller to do. */
enum reac_watch_act {
	/** Nothing to do — the verdict agrees with what this segment already is. */
	REAC_WATCH_STAND = 0,
	/** Somebody else masters our wire: master down, slave up, no shouting. */
	REAC_WATCH_YIELD,
	/** The rival we yielded to is gone: take the wire back as master. */
	REAC_WATCH_RETAKE,
	/** A refused segment's rival stopped mastering it: down with the door, up with
	 *  the segment, in the role the verdict now names. */
	REAC_WATCH_UNREFUSE,
};

const char *reac_watch_act_name(enum reac_watch_act a);

/**
 * Does this segment go on being classified after it is served?
 *
 * @param served  the verdict the segment was served on.
 * @param pinned  the operator answered for this wire (`REAC_ROLE_<segment>`).
 *
 * An unpinned MASTER does: the wire is ours only while nobody else claims it. A REFUSED
 * one does whether or not it is pinned, because a refusal with no engine behind it has
 * nothing else that could ever notice the rival leaving (0.5.1's door). A pinned master
 * does not — a pin is an answer, and the one thing a rival can do to it is the 0.5.1
 * refusal, which is decided by that segment's own engine. A wire we JOINED was never
 * ours to lose, and an undecided one is not served at all.
 */
int reac_watch_keep(enum reac_hunt_verdict served, int pinned);

/** What the caller knows about one served segment when its verdict changes. */
struct reac_watch_in {
	/** This segment is a published DOOR: refused, no engine behind it (0.5.1). */
	int door;
	/** Our engine on this segment is the MASTER role right now. */
	int we_master;
	/** `REAC_ROLE_<segment>` answered for this wire. */
	int pinned;
	/** What the kept sniffer's hunt says about the wire NOW. */
	enum reac_hunt_verdict verdict;
	/** The poll's clock, read once at the top of the poll. */
	uint64_t now_ns;
	/** When this segment's sniffer opened — the door's dwell is measured from it. */
	uint64_t opened_ns;
};

/**
 * The whole table, and it never latches in either direction.
 *
 * A DOOR WAITS OUT THE DISCOVERY TABLE'S OWN WITHDRAWAL WINDOW before it comes down.
 * Its sniffer was opened when the door went up, so for its first moments it has heard
 * nothing at all — and a pinned master on a silent wire decides MASTER at once, by
 * design. Undoing the refusal on that took the door down 200 ms after putting it up and
 * put it back a second later when the box's next master record arrived: measured, as
 * exactly that flap. `REAC_DISCO_STALE_NS` is what "the rival is really gone" means
 * everywhere else in this daemon, so it is the bar here too.
 *
 * A YIELD AND A RETAKE NEED NO DWELL OF THEIR OWN. Their sniffer has been listening
 * since the segment came up, so its table is warm: a verdict that has moved has moved
 * because a sighting arrived, or because one aged out under that same 5 s window. The
 * dwell is already inside the evidence.
 */
enum reac_watch_act reac_watch_decide(const struct reac_watch_in *in);

#endif /* REAC_WATCH_H */
