// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_role_swap — the LIFECYCLE of a `reac.cfg.role` change
 * (2026-08-20-reac-master-arbitration.md §8). reac_role_cfg's own test covers
 * the decision (parse, same-role no-op, the accepted answer); this one covers
 * what the segment says AFTERWARDS, across the cross-engine swap the decision
 * asks for.
 *
 * THE PROPERTY UNDER TEST IS THAT `applied` IS UNREACHABLE UNTIL THE NEW ENGINE
 * IS DOING THE JOB. Every window between the assertion and that moment answers
 * something else, and the two roles reach "doing the job" by different rules —
 * a master by pacing its own wire, a slave only by being enrolled. A slave on a
 * quiet wire never leaves the hunt, and the answer must say so.
 *
 * The lock half is a REAL mechanism test, not a model: reac_seglock's abstract
 * socket is claimed, proven to EXCLUDE a second claimant, released, and proven
 * to be claimable again — which is exactly "the old engine's lock is gone and
 * the other engine can own the segment" at the only level an offline test can
 * establish it. It runs on `lo`, whose lock nothing else on a host ever takes,
 * and it binds an inert abstract name: no packet is sent and no REAC segment is
 * touched.
 *
 * WHAT THIS TEST DOES NOT ESTABLISH: that a real Roland box or desk re-attaches
 * across a swap. No capture shows a desk ceding a segment, so that claim has no
 * evidence here or anywhere in the corpus — it is an operator-present rig test
 * (reac_role_swap.h's closing note). */
#include "reac_role_swap.h"
#include "reac_role_cfg.h"
#include "reac_seglock.h"

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
#define CHK_STATE(s, eng, want) CHK(strcmp(reac_role_swap_state((s), (eng)), (want)) == 0)

/* ---- the answer table, exhaustively ------------------------------------- */

static int test_answer_table(void)
{
	struct reac_role_swap s;

	/* Nothing up: the segment is owned by neither engine, whatever it was
	 * last asked for. */
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	CHK(s.engine_up == 0);
	CHK_STATE(&s, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_REESTABLISH_PENDING);
	CHK_STATE(&s, REAC_ROLE_ENGINE_HUNTING, REAC_ROLE_STATE_REESTABLISH_PENDING);
	CHK_STATE(&s, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);

	/* The asked-for engine is up and performing: applied, and only here. */
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK_STATE(&s, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_APPLIED);
	/* Up but not yet performing: hunting, never applied. */
	CHK_STATE(&s, REAC_ROLE_ENGINE_HUNTING, REAC_ROLE_STATE_HUNTING);
	/* An engine that reports itself down while the record says up is still a
	 * segment carrying nothing — pending, not a stale applied. */
	CHK_STATE(&s, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);

	/* The WRONG engine is up: the swap is owed, so no reading of that engine
	 * may answer applied — it is doing a job nobody asked for. */
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) != 0);
	CHK_STATE(&s, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_REESTABLISH_PENDING);
	CHK_STATE(&s, REAC_ROLE_ENGINE_HUNTING, REAC_ROLE_STATE_REESTABLISH_PENDING);
	return 0;
}

/* ---- the same-role assertion stays the existing no-op -------------------- */

static int test_same_role_is_a_no_op(void)
{
	struct reac_role_swap s;

	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	/* Asserting the role already running asks for no swap and moves the answer
	 * nowhere: it was already the fact. */
	CHK(reac_role_swap_request(&s, REAC_ROLE_MASTER) == 0);
	CHK(s.running == REAC_ROLE_MASTER);
	CHK(s.engine_up == 1);
	CHK_STATE(&s, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_APPLIED);

	/* Same, from the slave side, and a slave's no-op does not promote it out of
	 * the hunt: the assertion changed nothing, so neither does the answer. */
	reac_role_swap_init(&s, REAC_ROLE_SLAVE);
	reac_role_swap_opened(&s, REAC_ROLE_SLAVE);
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) == 0);
	CHK_STATE(&s, REAC_ROLE_ENGINE_HUNTING, REAC_ROLE_STATE_HUNTING);

	/* With NO engine up, even the same role owes an open — nothing is running
	 * to be the no-op's already-true fact. */
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	CHK(reac_role_swap_request(&s, REAC_ROLE_MASTER) != 0);
	return 0;
}

/* ---- the engines' own honest readings ------------------------------------ */

static int test_master_performs_by_pacing(void)
{
	/* A master with nothing plugged in is mastering: it holds the lock, it
	 * paces, it probes. PROBING and beyond is the role fulfilled. */
	CHK(reac_role_engine_of_master(1, REAC_M_PROBING) == REAC_ROLE_ENGINE_PERFORMING);
	CHK(reac_role_engine_of_master(1, REAC_M_GRANTING) == REAC_ROLE_ENGINE_PERFORMING);
	CHK(reac_role_engine_of_master(1, REAC_M_ESTABLISHED) == REAC_ROLE_ENGINE_PERFORMING);
	/* IDLE is the transient before the first emitted frame — honest, not applied. */
	CHK(reac_role_engine_of_master(1, REAC_M_IDLE) == REAC_ROLE_ENGINE_HUNTING);
	/* No engine, no reading. */
	CHK(reac_role_engine_of_master(0, REAC_M_ESTABLISHED) == REAC_ROLE_ENGINE_DOWN);
	return 0;
}

static int test_slave_hunts_until_enrolled(void)
{
	/* THE CASE THE OPERATOR MEETS: a recorder asked for on a wire with no desk.
	 * The slave engine is up, flooding and cold-connecting, and that is the
	 * honest terminal — never "established", never "applied". */
	CHK(reac_role_engine_of_slave(1, 0) == REAC_ROLE_ENGINE_HUNTING);
	CHK(reac_role_engine_of_slave(1, 1) == REAC_ROLE_ENGINE_PERFORMING);
	CHK(reac_role_engine_of_slave(0, 1) == REAC_ROLE_ENGINE_DOWN);
	return 0;
}

/* ---- the round trip, answer by answer ------------------------------------ */

static int test_round_trip_master_slave_master(void)
{
	struct reac_role_swap s;
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK_STATE(&s, reac_role_engine_of_master(s.engine_up, REAC_M_ESTABLISHED),
	          REAC_ROLE_STATE_APPLIED);

	/* 1. The console asks for the recorder end. Accepted, swap owed. */
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) != 0);
	CHK_STATE(&s, reac_role_engine_of_master(s.engine_up, REAC_M_ESTABLISHED),
	          REAC_ROLE_STATE_REESTABLISH_PENDING);

	/* 2. The master engine goes down — socket, pacer thread and segment lock
	 *    with it. Nothing owns the segment. */
	reac_role_swap_closed(&s);
	CHK_STATE(&s, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);

	/* 3. The slave engine comes up on a quiet wire: hunting, and it says so. */
	reac_role_swap_opened(&s, REAC_ROLE_SLAVE);
	CHK_STATE(&s, reac_role_engine_of_slave(s.engine_up, 0), REAC_ROLE_STATE_HUNTING);

	/* 4. A desk enrols it: NOW the role is performed. */
	CHK_STATE(&s, reac_role_engine_of_slave(s.engine_up, 1), REAC_ROLE_STATE_APPLIED);

	/* 5. And back again — the operator takes the wire back. */
	CHK(reac_role_swap_request(&s, REAC_ROLE_MASTER) != 0);
	CHK_STATE(&s, reac_role_engine_of_slave(s.engine_up, 1),
	          REAC_ROLE_STATE_REESTABLISH_PENDING);
	reac_role_swap_closed(&s);
	CHK_STATE(&s, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK_STATE(&s, reac_role_engine_of_master(s.engine_up, REAC_M_PROBING),
	          REAC_ROLE_STATE_APPLIED);
	return 0;
}

/* A re-open that FAILS leaves the segment down, and the answer must keep saying
 * so rather than settling on the role nobody is performing. */
static int test_failed_reopen_never_settles(void)
{
	struct reac_role_swap s;
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) != 0);
	reac_role_swap_closed(&s);
	/* listener_open refused (no CAP_NET_RAW for the slave socket, say): nothing
	 * is opened, so nothing calls _opened, and the answer stays pending for as
	 * long as that is the truth. */
	CHK_STATE(&s, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);
	CHK_STATE(&s, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_REESTABLISH_PENDING);
	return 0;
}

/* Two assertions inside one drain window must not cancel into a no-op that
 * leaves the wrong engine running: the swap is owed against what is UP. */
static int test_two_assertions_settle_against_the_live_engine(void)
{
	struct reac_role_swap s;
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) != 0);
	/* Asked back before the re-open ran: the master engine is still the one up,
	 * so nothing is owed and the answer is the fact again. */
	CHK(reac_role_swap_request(&s, REAC_ROLE_MASTER) == 0);
	CHK_STATE(&s, reac_role_engine_of_master(s.engine_up, REAC_M_PROBING),
	          REAC_ROLE_STATE_APPLIED);
	return 0;
}

/* ---- head-amp is the master's alone, across the swap --------------------- */

static int test_headamp_is_master_only(void)
{
	CHK(reac_role_emits_headamp(REAC_ROLE_MASTER) != 0);
	CHK(reac_role_emits_headamp(REAC_ROLE_SLAVE) == 0);

	/* Walked across the round trip: whatever the segment answers, the side that
	 * may put head-amp on the wire is decided by the RUNNING engine and by
	 * nothing else — an assertion in flight never licenses an emission. */
	struct reac_role_swap s;
	reac_role_swap_init(&s, REAC_ROLE_MASTER);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK(reac_role_emits_headamp(s.running) != 0);
	CHK(reac_role_swap_request(&s, REAC_ROLE_SLAVE) != 0);
	CHK(reac_role_emits_headamp(s.running) != 0);   /* still the master engine */
	reac_role_swap_closed(&s);
	reac_role_swap_opened(&s, REAC_ROLE_SLAVE);
	CHK(reac_role_emits_headamp(s.running) == 0);   /* a box never tells its desk */
	reac_role_swap_request(&s, REAC_ROLE_MASTER);
	CHK(reac_role_emits_headamp(s.running) == 0);   /* not until the engine is back */
	reac_role_swap_closed(&s);
	reac_role_swap_opened(&s, REAC_ROLE_MASTER);
	CHK(reac_role_emits_headamp(s.running) != 0);
	return 0;
}

/* ---- the segment lock really changes hands ------------------------------- */

/* The mechanism, not a model of it: what listener_close releases and what the
 * next listener_open claims is this same abstract socket. `lo` is a segment no
 * REAC master ever drives, so the name is inert; nothing is transmitted. */
static int test_segment_lock_changes_hands(void)
{
	struct reac_seglock held, rival;
	held.fd = -1;
	rival.fd = -1;

	/* PRESENCE BEFORE ABSENCE: prove the lock can be taken at all before any
	 * conclusion is drawn from a refusal. */
	int claimed = reac_seglock_claim(&held, "lo");
	if (claimed == -2) {
		fprintf(stderr, "FAIL: the segment lock could not be identified for lo "
		        "(no /proc/self/ns/net?) — this test cannot conclude anything\n");
		return 1;
	}
	CHK(claimed == 0);
	CHK(held.fd >= 0);

	/* POSITIVE CONTROL for the refusal: while it is held, a second engine
	 * asking for the same segment is refused. Without this, the release check
	 * below would pass against a lock that never excluded anybody. */
	CHK(reac_seglock_claim(&rival, "lo") == -1);
	CHK(rival.fd < 0);

	/* The old engine goes down: the socket is closed and the name is free. */
	reac_seglock_release(&held);
	CHK(held.fd < 0);

	/* The other engine can now own the segment. */
	CHK(reac_seglock_claim(&rival, "lo") == 0);
	CHK(rival.fd >= 0);

	/* And back again, the same way. */
	reac_seglock_release(&rival);
	CHK(reac_seglock_claim(&held, "lo") == 0);
	reac_seglock_release(&held);
	return 0;
}

/* A SLAVE TAKES NO SEGMENT LOCK — driving is what claims a segment, and a slave
 * does not drive. So after a swap to slave the lock is free for the real master
 * on the wire, which is the whole point of yielding. */
static int test_only_the_master_holds_the_segment(void)
{
	struct reac_seglock ours, theirs;
	ours.fd = -1;
	theirs.fd = -1;

	/* Master role: claimed. */
	CHK(reac_role_emits_headamp(REAC_ROLE_MASTER) != 0);
	CHK(reac_seglock_claim(&ours, "lo") == 0);
	CHK(reac_seglock_claim(&theirs, "lo") == -1);   /* the segment is ours */

	/* Swap to slave: listener_close releases it and listener_open's slave
	 * branch claims nothing (main.c: the claim sits inside the master branch,
	 * immediately before the first frame). The wire's real master may take it. */
	reac_seglock_release(&ours);
	CHK(reac_seglock_claim(&theirs, "lo") == 0);
	reac_seglock_release(&theirs);
	return 0;
}

int main(void)
{
	if (test_answer_table()) return 1;
	if (test_same_role_is_a_no_op()) return 1;
	if (test_master_performs_by_pacing()) return 1;
	if (test_slave_hunts_until_enrolled()) return 1;
	if (test_round_trip_master_slave_master()) return 1;
	if (test_failed_reopen_never_settles()) return 1;
	if (test_two_assertions_settle_against_the_live_engine()) return 1;
	if (test_headamp_is_master_only()) return 1;
	if (test_segment_lock_changes_hands()) return 1;
	if (test_only_the_master_holds_the_segment()) return 1;
	printf("OK: reac_role_swap — the swap answers honestly in every window, "
	       "the slave hunt never reads as applied, head-amp stays the master's, "
	       "and the segment lock really changes hands\n");
	return 0;
}
