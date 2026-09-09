// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// A SEGMENT THAT IS UP CAN STILL BE RE-DECIDED, and this is the table that says how
// (DESIGN.md, 0.5.4). The decision used to live inline in main.c's hearing_yield, where
// the only way to exercise it was a 70-second veth run that cannot choose which route
// took the wire -- so the case the release is about (a wire won because a BOX was heard)
// was untestable by construction. Here it is one call with its inputs written down.

#include "reac_watch.h"
#include <reac/reac_disco.h>

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { \
	if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	               fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; } \
} while (0)

static enum reac_watch_act act(int door, int we_master, int pinned,
                               enum reac_hunt_verdict v, uint64_t now, uint64_t opened)
{
	struct reac_watch_in in;
	memset(&in, 0, sizeof in);
	in.door = door;
	in.we_master = we_master;
	in.pinned = pinned;
	in.verdict = v;
	in.now_ns = now;
	in.opened_ns = opened;
	return reac_watch_decide(&in);
}

/* WHICH SERVED SEGMENTS GO ON BEING CLASSIFIED. Won on silence or won on a box heard,
 * the answer is the same since 0.5.4 -- it is the wire's PIN that decides, not the
 * evidence we won it with. */
static void test_keep(void)
{
	CHECK(reac_watch_keep(REAC_HUNT_MASTER, 0) == 1, "an unpinned master keeps its sniffer");
	CHECK(reac_watch_keep(REAC_HUNT_MASTER, 1) == 0, "a PINNED master keeps its role, so it "
	      "has nothing to re-decide");
	CHECK(reac_watch_keep(REAC_HUNT_REFUSED, 0) == 1, "a refusal must be able to end");
	CHECK(reac_watch_keep(REAC_HUNT_REFUSED, 1) == 1, "a pinned wire's refusal must be able "
	      "to end too -- that is the 0.5.1 door");
	CHECK(reac_watch_keep(REAC_HUNT_SLAVE, 0) == 0, "we never had this wire; there is "
	      "nothing of ours to yield");
	CHECK(reac_watch_keep(REAC_HUNT_HUNTING, 0) == 0, "an undecided wire is not served at all");
}

/* THE YIELD. A desk (or a box on M) starts mastering a wire we are driving, and we get
 * out of its way -- whichever way we came to be driving it. */
static void test_yield(void)
{
	CHECK(act(0, 1, 0, REAC_HUNT_SLAVE, 100, 0) == REAC_WATCH_YIELD,
	      "a master on our unpinned wire is yielded to");
	CHECK(act(0, 1, 1, REAC_HUNT_SLAVE, 100, 0) == REAC_WATCH_STAND,
	      "a PIN stays: the operator answered for this wire");
	CHECK(act(0, 0, 0, REAC_HUNT_SLAVE, 100, 0) == REAC_WATCH_STAND,
	      "already the slave: yielding twice is a flap");
}

/* THE RETAKE, which is the venue case. The desk is switched off at the end of the night
 * and the wire has to come back, or the console's boxes are on a segment nobody drives. */
static void test_retake(void)
{
	CHECK(act(0, 0, 0, REAC_HUNT_MASTER, 100, 0) == REAC_WATCH_RETAKE,
	      "the rival stopped mastering the wire we had yielded: take it back");
	CHECK(act(0, 1, 0, REAC_HUNT_MASTER, 100, 0) == REAC_WATCH_STAND,
	      "already the master: the verdict stands");
	CHECK(act(0, 0, 1, REAC_HUNT_MASTER, 100, 0) == REAC_WATCH_STAND,
	      "a pinned slave is the operator's answer, not a yield to undo");
	CHECK(act(0, 0, 0, REAC_HUNT_HUNTING, 100, 0) == REAC_WATCH_STAND,
	      "an undecided wire decides nothing");
	CHECK(act(0, 0, 0, REAC_HUNT_REFUSED, 100, 0) == REAC_WATCH_STAND,
	      "a rival we cannot read does not hand the wire back");
}

/* THE DOOR (0.5.1): a refused segment is published with no engine, and the refusal ends
 * when the rival does. Unchanged by this release, and moved here so one file holds the
 * whole law -- including the dwell that stopped it flapping. */
static void test_door(void)
{
	const uint64_t stale = REAC_DISCO_STALE_NS;
	CHECK(act(1, 0, 1, REAC_HUNT_REFUSED, stale * 3, 0) == REAC_WATCH_STAND,
	      "still refused: the door stands");
	CHECK(act(1, 0, 1, REAC_HUNT_MASTER, stale * 3, 0) == REAC_WATCH_UNREFUSE,
	      "the rival is gone past the dwell: down with the door");
	CHECK(act(1, 0, 1, REAC_HUNT_MASTER, stale - 1, 0) == REAC_WATCH_STAND,
	      "AN EMPTY TABLE IS NOT EVIDENCE THAT THE RIVAL LEFT: a door just opened has "
	      "heard nothing yet, and undoing the refusal on that is the measured flap");
	/* UNSIGNED TIME COMPARES IN THE RIGHT ORDER OR NOT AT ALL. The poll reads `now` once
	 * at the top and a sniffer opened inside that same poll stamps a LATER one, so the
	 * subtraction wrapped to ~584 years and the dwell passed on the first poll -- a door
	 * that came down 200 ms after going up, measured. */
	CHECK(act(1, 0, 1, REAC_HUNT_MASTER, 1000, 2000) == REAC_WATCH_STAND,
	      "a door opened AFTER the poll's clock reading must not wrap into a pass");
	CHECK(act(1, 0, 0, REAC_HUNT_SLAVE, stale * 3, 0) == REAC_WATCH_UNREFUSE,
	      "an unpinned door whose rival became joinable comes down too -- the serve path "
	      "takes the verdict from there");
}

int main(void)
{
	test_keep();
	test_yield();
	test_retake();
	test_door();
	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("test_reac_watch: all checks passed\n");
	return 0;
}
