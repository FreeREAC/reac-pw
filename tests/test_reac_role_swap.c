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
#include <reac/transport/reac_role_swap.h>
#include "reac_role_cfg.h"
#include <reac/transport/reac_seglock.h>
#include <reac/transport/reac_segment_ident.h>   /* W1: the segment's identity + a slave's answer */
#include <reac/reac_arbitration.h>     /* the vocabulary those answers must BE */
#include <reac/reac.h>            /* REAC_MAX_CHANNELS — a desk's downstream width */

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

/* ======== W1: THE SEGMENT IS THE SAME ADDRESSABLE FACT IN BOTH ROLES ========
 *
 * docs/SLAVE-EMULATION-SCOPE.md W1. The console addressed a segment by parsing
 * its reac-playback node name, and a recorder has no reac-playback node — so a
 * role could be driven one way and never back. The identity is now DECLARED, and
 * a recorder answers with the same aggregate a mixer does. These pin the pieces
 * that decide both. */

/* The identity is the instance name, and the bare segment is named rather than
 * left as an empty string a reader would have to interpret. */
static int test_segment_names_itself(void)
{
	CHK(strcmp(reac_segment_name("enp131s0"), "enp131s0") == 0);
	CHK(strcmp(reac_segment_name(NULL), REAC_SEGMENT_NAME_DEFAULT) == 0);
	CHK(strcmp(reac_segment_name(""), REAC_SEGMENT_NAME_DEFAULT) == 0);
	/* The value a node stamps is EXACTLY what a reader keys its row on, so the
	 * default must be the literal the console addresses, not a near-miss. */
	CHK(strcmp(REAC_SEGMENT_NAME_DEFAULT, "default") == 0);
	CHK(strcmp(REAC_PROP_SEGMENT, "reac.segment") == 0);
	return 0;
}

/* THE LATCH DECAYS, and that is the whole reason it is a latch. reac_rx counts
 * cumulatively and never counts down, so "we once decoded a master frame" would
 * report a desk that was unplugged an hour ago — reac_disco ages its sightings
 * for exactly this and the two must decay on the same rule. */
static int test_heard_decays_when_the_desk_goes_quiet(void)
{
	struct reac_segment_heard h;
	reac_segment_heard_init(&h, 0);
	CHK(h.heard == 0);              /* nothing heard yet, whatever the counter reads */

	/* PRESENCE BEFORE ABSENCE: prove the latch can SEE a master before any
	 * assertion about it going quiet means anything. */
	CHK(reac_segment_heard_step(&h, 1, 5) == 1);
	CHK(reac_segment_heard_step(&h, 4000, 5) == 1);

	/* The count stops moving. The claim must survive a tick or two of jitter and
	 * then go — never stand on the frozen number for ever. */
	for (int i = 0; i < 4; i++)
		CHK(reac_segment_heard_step(&h, 4000, 5) == 1);
	CHK(reac_segment_heard_step(&h, 4000, 5) == 0);
	CHK(reac_segment_heard_step(&h, 4000, 5) == 0);   /* and stays gone */

	/* The desk comes back: one new frame is enough, and it is heard again. */
	CHK(reac_segment_heard_step(&h, 4001, 5) == 1);

	/* A seeded latch does not inherit the previous engine's evidence: seeding at
	 * a non-zero count is "we have heard nothing YET", not "we heard 4001". */
	reac_segment_heard_init(&h, 4001);
	CHK(h.heard == 0);
	CHK(reac_segment_heard_step(&h, 4001, 5) == 0);   /* unmoved: still nothing */
	CHK(reac_segment_heard_step(&h, 4002, 5) == 1);   /* moved: now something */

	/* The shipped bar is reac_disco's own 5 s withdrawal, at main's 200 ms poll. */
	CHK(REAC_SEGMENT_HEARD_QUIET_TICKS == 25);
	return 0;
}

/* THE PARITY TABLE. Every value is asserted against reac_arbitration's OWN
 * namers, never against a string literal spelled a second time here — a test
 * that re-spelled them would pass while the two vocabularies drifted apart. */
static int test_slave_answers_the_segment_aggregate(void)
{
	struct reac_segment_answer a;
	const uint8_t desk[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
	uint64_t mac48 = reac_mac48_pack(desk);

	/* HUNTING: a recorder on a quiet wire. Nothing is mastering, nothing paces,
	 * and there is no MAC to name — every one of those is a fact, not a gap. */
	reac_segment_answer_slave(&a, 0, 0, 96000, REAC_MAX_CHANNELS);
	CHK(strcmp(a.master_state, reac_segment_master_name(REAC_SEGMENT_NONE)) == 0);
	CHK(strcmp(a.master_mac, "none") == 0);
	CHK(strcmp(a.pace_source, reac_pace_source_name(REAC_PACE_FREE_RUN)) == 0);
	CHK(strcmp(a.rival_kind, reac_rival_kind_name(REAC_RIVAL_NONE)) == 0);
	CHK(strcmp(a.refusal, reac_rival_refusal(REAC_RIVAL_NONE)) == 0);
	CHK(strcmp(a.conflict, "0") == 0);
	CHK(strcmp(a.rate, "96000") == 0);

	/* ENROLLED: a desk is heard. The gate that passed those frames accepts the
	 * 40-channel downstream and nothing else, so the geometry is a DESK by the
	 * master's own classifier — and that matters beyond tidiness, because the
	 * console's role policy joins a desk and refuses everything else. A recorder
	 * that answered `none` here would be reported as refusing the very master it
	 * is happily joined to. */
	reac_segment_answer_slave(&a, 1, mac48, 48000, REAC_MAX_CHANNELS);
	CHK(strcmp(a.master_state, reac_segment_master_name(REAC_SEGMENT_FOREIGN)) == 0);
	CHK(strcmp(a.master_mac, "00:40:ab:c4:80:3b") == 0);
	CHK(strcmp(a.pace_source, reac_pace_source_name(REAC_PACE_FOREIGN_MASTER)) == 0);
	CHK(strcmp(a.rival_kind, reac_rival_kind_name(REAC_RIVAL_DESK)) == 0);
	CHK(strcmp(a.refusal, reac_rival_refusal(REAC_RIVAL_DESK)) == 0);
	CHK(strcmp(a.rate, "48000") == 0);

	/* A joined desk is not a refusal and not a conflict: the first is what
	 * reac_rival_refusal says of a desk, the second is definitional — the flag
	 * means a foreign master is live WHILE WE MASTER, and a slave does not. */
	CHK(strcmp(a.refusal, "none") == 0);
	CHK(strcmp(a.conflict, "0") == 0);

	/* Heard, but no MAC learned yet (the grant burst has not named a master):
	 * the presence is reported and the identity is honestly absent. */
	reac_segment_answer_slave(&a, 1, 0, 48000, REAC_MAX_CHANNELS);
	CHK(strcmp(a.master_state, reac_segment_master_name(REAC_SEGMENT_FOREIGN)) == 0);
	CHK(strcmp(a.master_mac, "none") == 0);

	/* JOINED TO A BOX ON M (0.5.1). The same segment, the same engine, a different
	 * peer: an S-0808 strapped to master streams 8 channels, we follow its clock,
	 * and the answer says `box` because the WIDTH says box. It is not a refusal —
	 * we joined it — and publishing the box's refusal CODE here would report this
	 * segment as declining the very master it is following. */
	reac_segment_answer_slave(&a, 1, mac48, 48000, 8);
	CHK(strcmp(a.master_state, reac_segment_master_name(REAC_SEGMENT_FOREIGN)) == 0);
	CHK(strcmp(a.rival_kind, reac_rival_kind_name(REAC_RIVAL_BOX)) == 0);
	CHK(strcmp(a.refusal, reac_rival_refusal(REAC_RIVAL_NONE)) == 0);
	CHK(strcmp(a.refusal, "none") == 0);
	CHK(strcmp(a.pace_source, reac_pace_source_name(REAC_PACE_FOREIGN_MASTER)) == 0);
	CHK(strcmp(a.conflict, "0") == 0);

	/* THE REFUSED DOOR — a wire pinned MASTER with a box mastering it. No engine
	 * runs behind this answer; the node exists so the refusal can be SEEN, which
	 * is the whole defect of 2026-09-09 (the refused wire published nothing and
	 * vanished from the console). The MAC is the RIVAL's, never ours. */
	reac_segment_answer_refused(&a, 8, mac48, 48000);
	CHK(strcmp(a.master_state, reac_segment_master_name(REAC_SEGMENT_FOREIGN)) == 0);
	CHK(strcmp(a.master_mac, "00:40:ab:c4:80:3b") == 0);
	CHK(strcmp(a.rival_kind, reac_rival_kind_name(REAC_RIVAL_BOX)) == 0);
	CHK(strcmp(a.refusal, reac_rival_refusal(REAC_RIVAL_BOX)) == 0);
	CHK(strcmp(a.refusal, "rival-master-box") == 0);
	CHK(strcmp(a.pace_source, reac_pace_source_name(REAC_PACE_FOREIGN_MASTER)) == 0);
	CHK(strcmp(a.conflict, "0") == 0);
	CHK(strcmp(a.rate, "48000") == 0);

	/* A rival with no readable geometry refuses under its own code, and the width
	 * that could not be read is 0 rather than a guess. */
	reac_segment_answer_refused(&a, 0, mac48, 96000);
	CHK(strcmp(a.rival_kind, reac_rival_kind_name(REAC_RIVAL_UNKNOWN)) == 0);
	CHK(strcmp(a.refusal, "rival-master-unknown") == 0);

	/* No rate published is "0", which a consumer reads as no rate — never a
	 * guess at one. */
	reac_segment_answer_slave(&a, 0, 0, 0, REAC_MAX_CHANNELS);
	CHK(strcmp(a.rate, "0") == 0);
	reac_segment_answer_slave(&a, 0, 0, -1, REAC_MAX_CHANNELS);
	CHK(strcmp(a.rate, "0") == 0);
	return 0;
}

/* The packed MAC is a ROUND TRIP, not a formatting convenience: the engine
 * thread stores it and the publish timer reads it, so a byte lost in the pack
 * would be a wrong master silently published. */
static int test_mac48_round_trips(void)
{
	const uint8_t in[6] = { 0xc4, 0x06, 0x80, 0x00, 0xff, 0x01 };
	uint8_t out[6];
	reac_mac48_unpack(reac_mac48_pack(in), out);
	CHK(memcmp(in, out, 6) == 0);

	/* The high byte must survive: a 48-bit value shifted through a 32-bit
	 * intermediate would lose exactly this one. */
	const uint8_t high[6] = { 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
	reac_mac48_unpack(reac_mac48_pack(high), out);
	CHK(memcmp(high, out, 6) == 0);
	CHK(reac_mac48_pack(high) == 0xff0000000000ull);

	/* An unlearned master packs to 0, which is what "none" is published from. */
	const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
	CHK(reac_mac48_pack(zero) == 0);
	return 0;
}

/* THE ROUND TRIP, ANSWER BY ANSWER, ACROSS BOTH NODES. The swap test above walks
 * the role state; this walks what a CONSOLE reads at each phase — which is the
 * half W1 was missing. The point is that at no phase is the segment unaddressable:
 * whichever node exists carries the identity and an answer. */
static int test_the_segment_answers_in_both_roles(void)
{
	struct reac_role_swap sw;
	struct reac_segment_answer a;
	const char *inst = "enp131s0";

	/* MIXER. The identity is on the reac-playback sink; the aggregate comes from
	 * the pacer's arbitration, which this module does not compute — what W1 pins
	 * here is that the segment is NAMED, by the same function both nodes call. */
	reac_role_swap_init(&sw, REAC_ROLE_MASTER);
	reac_role_swap_opened(&sw, REAC_ROLE_MASTER);
	CHK_STATE(&sw, REAC_ROLE_ENGINE_PERFORMING, REAC_ROLE_STATE_APPLIED);
	CHK(strcmp(reac_segment_name(inst), "enp131s0") == 0);

	/* -> RECORDER. The sink is destroyed, so the identity and the answer move to
	 * the capture node — the SAME name, which is the whole point: a console that
	 * keyed on it addresses the same segment across the swap. */
	CHK(reac_role_swap_request(&sw, REAC_ROLE_SLAVE) != 0);
	reac_role_swap_closed(&sw);
	CHK_STATE(&sw, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);
	reac_role_swap_opened(&sw, REAC_ROLE_SLAVE);
	CHK(strcmp(reac_segment_name(inst), "enp131s0") == 0);

	/* Quiet wire: hunting, and the aggregate says why — nothing masters it. */
	reac_segment_answer_slave(&a, 0, 0, 96000, REAC_MAX_CHANNELS);
	CHK_STATE(&sw, reac_role_engine_of_slave(1, 0), REAC_ROLE_STATE_HUNTING);
	CHK(strcmp(a.master_state, "none") == 0);

	/* A desk arrives and enrols us: applied, and the aggregate names the desk. */
	reac_segment_answer_slave(&a, 1, reac_mac48_pack((const uint8_t[]){
		0x00, 0x40, 0xab, 0x11, 0x22, 0x33 }), 96000, REAC_MAX_CHANNELS);
	CHK_STATE(&sw, reac_role_engine_of_slave(1, 1), REAC_ROLE_STATE_APPLIED);
	CHK(strcmp(a.master_state, "foreign") == 0);
	CHK(strcmp(a.rival_kind, "desk") == 0);

	/* -> BACK TO MIXER. This is the gesture W1 exists to make reachable: the
	 * write lands on whichever node carries the door, and the segment re-opens
	 * as a master under the same name. */
	CHK(reac_role_swap_request(&sw, REAC_ROLE_MASTER) != 0);
	reac_role_swap_closed(&sw);
	CHK_STATE(&sw, REAC_ROLE_ENGINE_DOWN, REAC_ROLE_STATE_REESTABLISH_PENDING);
	reac_role_swap_opened(&sw, REAC_ROLE_MASTER);
	CHK_STATE(&sw, reac_role_engine_of_master(1, REAC_M_PROBING), REAC_ROLE_STATE_APPLIED);
	CHK(strcmp(reac_segment_name(inst), "enp131s0") == 0);

	/* Head-amp went with the role, not with the node it used to hang off. */
	CHK(reac_role_emits_headamp(REAC_ROLE_MASTER) != 0);
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
	if (test_segment_names_itself()) return 1;
	if (test_heard_decays_when_the_desk_goes_quiet()) return 1;
	if (test_slave_answers_the_segment_aggregate()) return 1;
	if (test_mac48_round_trips()) return 1;
	if (test_the_segment_answers_in_both_roles()) return 1;
	printf("OK: reac_role_swap — the swap answers honestly in every window, "
	       "the slave hunt never reads as applied, head-amp stays the master's, "
	       "the segment lock really changes hands, and the segment names itself "
	       "and answers the aggregate in BOTH roles (W1)\n");
	return 0;
}
