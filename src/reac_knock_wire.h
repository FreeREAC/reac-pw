// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock_wire: A WIRE NOBODY PINNED IS LISTENED TO, THEN KNOCKED ON, NEVER FLOODED.
 *
 * THE DEFECT IT EXISTS AGAINST (#31, #40). libreac's masterless licence (reac_knock.h) proves
 * that a wire which carried no REAC frame for 500 ms has no master on it, and until this module
 * the daemon answered that proof by taking the wire as MASTER: the whole stream, 4000 or 8000
 * frames a second, onto a wire on which nothing had ever been heard. On the untagged native
 * VLAN of a trunk that is ~96 Mbit/s of broadcast poured into two switches' management VLAN,
 * and a managed switch shut the port every 17 s for as long as the daemon ran (desk,
 * 2026-10-24). Operator ruling 2026-10-25 (#40): a master never floods a wire searching for a
 * box; a box straight on a NIC stays supported, and the rule is the METHOD.
 *
 * WHAT A REAL BOX WAS SEEN TO ANSWER, AND WHAT WAS NEVER SEEN (docs/KNOCK.md has every
 * citation). A cold box does not answer a lone announce (libreac reac_knock.h:21-25). It
 * answers a master that is DRIVING, the continuous probing stream (reac_knock.h:25-27): an
 * S-0808 declared itself 0.70 s after such a stream opened, 17 ms after its first transfer's
 * final chunk (logs/master-default-20260826.log:6,11). In that stream every slot that is not
 * a control slot is an audio FILLER (libreac reac_master.c:1266,1297), and so is a real desk's
 * (libreac tests/corpus-baseline.txt:54: 269 701 of 270 593 frames). NO RECORD shows a real
 * box answering the control frames with the FILLER slots left empty, and none shows one
 * refusing them. libreac's fake_box answers them because it ignores FILLER by construction
 * (tools/fake_box.c:230-254), which proves the knock's sequence and nothing about a box.
 *
 * SO THE KNOCK IS A DECLARED PLAN OF TWO CONTENTS, BOTH libreac's OWN MASTER. A `struct
 * reac_master` is stepped slot by slot at the wire's cadence exactly as the pacer steps it,
 * through one hunting cycle per knock, opening on the transfer's header. A CONTROL knock sends
 * the frames the master stamps with a control block and leaves every FILLER slot empty: the
 * bounded knock, unproven on a real box. A STREAM knock sends every slot, FILLER included:
 * the content a real box was seen to answer, for ONE cycle and at most
 * REAC_KNOCK_WIRE_STREAM_KNOCKS times between two resets. Which knock carries which content is
 * the plan's table (reac_knock_plans), chosen per segment by `knock =` in reac-pw.conf, so the
 * rig check changes it without a build. Nothing here encodes a frame of its own.
 *
 * AND IT BACKS OFF. Knock n of a spell opens REAC_KNOCK_WIRE_PERIOD(n) after knock n-1 did:
 * one cycle, doubled per unanswered knock, up to REAC_KNOCK_WIRE_PERIOD_MAX_NS. A frame heard
 * after a silence, or a link that came up (a fresh init), starts the plan and the period
 * again.
 *
 * THE FIRST EVIDENCE HEARD ENDS IT, and the stream starts only after a box answered. Evidence
 * is a peer whose settled role is a box or a master: a box's declaration, its JOIN, a desk.
 * A flood of unknown-role FILLER is a box that has just come up and is waiting to hear a
 * master, so it is knocked on through it. The answer is kept (REAC_KNOCK_WIRE_ANSWERS control
 * frames) because it is a fact about the push that produced it, and the caller hands the
 * kept frames to the new master before its pacer runs.
 *
 * A TRUNK PARENT IS NOT A SEGMENT (#39). It knocks untagged like any NIC until tagged REAC is
 * heard on one of its VLANs; from then on nothing is sent on it, the frames its socket hears
 * with their tags stripped are not answers, and it never becomes a segment.
 *
 * THE HOST'S NIC GETS NO STREAM (#46, 2026-10-26-a-host-nic-is-never-bounced-or-streamed-at).
 * A NIC that carries an address beyond link scope, a stacked sub-interface or a default route
 * is the host's (reac_link_owner.h); the desk's trunk parent is one, and the stream knock on
 * it is the flood of #31 for the length of a cycle, three times per spell. On such a wire a
 * STREAM rung is sent as a CONTROL knock and does not count against the cap; the caller says
 * so through reac_knock_wire_set_host, once a second, from the kernel's own answer. A bare
 * NIC, the direct-cable case, is unchanged.
 *
 * PURE: no sockets, no clock of its own, no allocation. The caller owns the sender (a deaf
 * AF_PACKET socket, libreac's reac_tx) and the timer; a test owns a fake wire. */
#ifndef REAC_KNOCK_WIRE_H
#define REAC_KNOCK_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include <reac/reac.h>          /* REAC_FRAME_BYTES */
#include <reac/reac_ctrlblk.h>  /* REAC_SCENE_STEPS */
#include <reac/reac_hunt.h>
#include <reac/reac_knock.h>
#include <reac/reac_master.h>

#include "reac_wake.h"

/* WHAT ONE KNOCK CARRIES: one hunting cycle of libreac's master, opening on the header. */
enum reac_knock_content {
	REAC_KNOCK_CONTROL = 0,  /* the control frames only; every FILLER slot left empty     */
	REAC_KNOCK_STREAM,       /* every slot, FILLER included: what a real box answered      */
};

/* A RUNG OF A PLAN: `knocks` knocks in a row carry `content`. */
struct reac_knock_rung {
	enum reac_knock_content content;
	unsigned knocks;
};

/* A PLAN: its rungs in order, then from the first again. `name` is the conf's word. */
struct reac_knock_plan {
	const char *name;
	const char *said;                    /* what the plan does, for the journal        */
	const struct reac_knock_rung *rung;
	int n_rung;
};

/* THE PLANS, declared once (reac_knock_wire.c). `escalate` is the default: two bounded
 * control knocks, then one cycle of the stream a box was seen to answer. `control` never
 * sends a FILLER; `stream` sends the stream cycle on every knock, inside the same caps. */
extern const struct reac_knock_plan reac_knock_plans[];
extern const int reac_knock_n_plans;
#define REAC_KNOCK_PLAN_DEFAULT "escalate"

/* The plan called `name` (case-insensitive), or NULL. */
const struct reac_knock_plan *reac_knock_plan_find(const char *name);

/* STREAM KNOCKS BETWEEN TWO RESETS. CHOSEN: three. A cold S-0808 answered the stream inside
 * its first cycle (logs/master-default-20260826.log:6,11), so one would do for the one box on
 * record; three cover a box that was mid-boot for the first. Past the cap a STREAM rung is
 * sent as CONTROL, so a wire with nothing on it sees at most this many stream cycles per
 * link-up, and the knock's wake ladder (REAC_WAKE_MAX_BOUNCES edges) bounds the link-ups. */
#define REAC_KNOCK_WIRE_STREAM_KNOCKS 3u

/* THE BACKOFF'S CEILING. CHOSEN: 30 s, so a wire with nothing on it carries one control
 * cycle every 30 s once the plan's stream knocks are spent: about 12 frames a second
 * averaged, against 4000 or 8000 for the stream. */
#define REAC_KNOCK_WIRE_PERIOD_MAX_NS (30ULL * 1000000000ULL)

/* A SLOT WHOSE TIME PASSED BY MORE THAN THIS IS NOT SENT LATE. CHOSEN: four probe strides
 * (500 chunks a second, reac_master.h REAC_M_BURST_PER_SEC). The main loop stalls for half a
 * second while it serves a segment (main.c hearing_hunt), and sending what fell due during
 * the stall back to back would bunch a cycle's frames into one burst. The master is still
 * stepped through those slots, so the cadence resumes in place; the push they belonged to is
 * interrupted, which a box ignores, and the next knock's is whole. */
#define REAC_KNOCK_WIRE_LATE_NS (8ULL * 1000000ULL)

/* The CONTROL frames kept from the answer, in order: the box's declaration or its JOIN, and
 * what follows before the poll serves the master. FILLER is not kept. */
#define REAC_KNOCK_WIRE_ANSWERS 4

enum reac_knock_wire_state {
	REAC_KNOCK_WIRE_LISTENING = 0, /* inside the masterless observation; nothing sent      */
	REAC_KNOCK_WIRE_KNOCKING,      /* proven masterless, nothing heard: knocks, and rests  */
	REAC_KNOCK_WIRE_ANSWERED,      /* a frame was heard while knocking: the knock is over  */
	REAC_KNOCK_WIRE_TRUNK,         /* tagged REAC on this parent's VLANs: not a segment    */
};

/* What the caller must DO, returned once per change. */
enum reac_knock_wire_act {
	REAC_KNOCK_WIRE_ACT_NONE = 0,
	REAC_KNOCK_WIRE_ACT_KNOCK,     /* open the sender and arm the timer; say it            */
	REAC_KNOCK_WIRE_ACT_HUSH,      /* a frame was heard: close the sender; say who         */
	REAC_KNOCK_WIRE_ACT_TRUNK,     /* this is a trunk parent: close the sender if open; say */
	REAC_KNOCK_WIRE_ACT_UNTRUNK,   /* the tags stopped: listening again from now            */
};

/* The wire. Returns 0 when the frame left, -1 when it did not. */
typedef int (*reac_knock_wire_send_fn)(void *ctx, const uint8_t *frame, size_t len);

struct reac_knock_wire {
	enum reac_knock_wire_state state;
	struct reac_knock licence;     /* libreac's masterless observation                     */
	struct reac_master master;     /* libreac's master, PROBING: its cycle is the knock    */
	uint8_t src[6];                /* the NIC's own address, the one the stream will use   */
	int fps;
	const struct reac_knock_plan *plan;
	int host;                      /* the NIC is the host's: a STREAM rung goes out as CONTROL */
	/* THE PLAN'S PLACE AND THE BACKOFF, reset by a frame after a silence and by init. */
	int rung;                      /* the rung the next knock is taken from                */
	unsigned rung_done;            /* knocks already taken from it                          */
	unsigned unanswered;           /* knocks since the reset: the backoff's exponent        */
	unsigned stream_knocks;        /* STREAM knocks since the reset, against the cap        */
	/* THIS KNOCK. */
	enum reac_knock_content content;
	int resting;                   /* between two knocks: nothing goes out until next_ns   */
	uint64_t next_ns;              /* when the next knock opens, while resting              */
	uint64_t t0_ns;                /* when slot 0 of this knock fell                        */
	uint64_t slot;                 /* the next slot the master is stepped through           */
	uint64_t due_ns;               /* when the next frame this knock sends falls due (0 = none) */
	/* WHAT LEFT, for the journal, the roster, the wake ladder and a test. */
	uint64_t sent;                 /* frames handed to the wire and accepted                */
	uint64_t spell_sent;           /* of which in this spell (since the licence's grant)   */
	uint64_t stream_sent;          /* of which in STREAM knocks                             */
	uint64_t stream_withheld;      /* STREAM rungs sent as CONTROL because the NIC is the host's */
	uint64_t send_errors;          /* frames the wire refused                               */
	uint64_t late;                 /* frames whose slot passed by REAC_KNOCK_WIRE_LATE_NS   */
	uint64_t pushes;               /* complete transfers sent: a FIRST, then the LAST       */
	uint64_t cycles;               /* knocks opened in this spell                           */
	int head_sent;                 /* this transfer's FIRST left, so its LAST completes it  */
	int chunks_sent;               /* MIDDLEs of this transfer that left                    */
	unsigned knocks;               /* spells: how many times the licence bought a knock     */
	uint64_t last_heard_ns;        /* the last frame heard here (0 = none), for the reset   */
	/* WHAT ANSWERED, and on which knock. */
	uint8_t answer[REAC_KNOCK_WIRE_ANSWERS][REAC_FRAME_BYTES];
	size_t answer_len[REAC_KNOCK_WIRE_ANSWERS];
	int n_answer;
	uint8_t answer_src[6];
	uint64_t answered_cycle;       /* the knock of the spell that was out (1-based)         */
	enum reac_knock_content answered_content;
};

/* A wire has just come up. `src` is the NIC's own address and `fps` the cadence the master
 * on it would run at; nothing is sent until the licence is granted. The plan is the
 * default until reac_knock_wire_set_plan says otherwise. */
void reac_knock_wire_init(struct reac_knock_wire *w, const uint8_t src[6], int fps,
                          uint64_t now_ns);

/* Use `plan` from the next knock on (NULL: the default). */
void reac_knock_wire_set_plan(struct reac_knock_wire *w, const struct reac_knock_plan *plan);

/* The kernel's answer about this NIC (reac_link_owner): 1 when it is the host's. From the
 * next knock on a STREAM rung is sent as CONTROL and counted in `stream_withheld`. */
void reac_knock_wire_set_host(struct reac_knock_wire *w, int host);

/* One step of the poll. `hunt` is the wire's verdict-in-progress: a PINNED wire is never
 * knocked (a pin serves on link, reac_hunt.h). `trunk_now` is the topology tap's answer:
 * tagged REAC heard on this parent's VLANs, fresh. The masterless licence is NEVER handed to
 * the hunt: a wire is mastered on the evidence of a box heard on it, never on its silence. */
enum reac_knock_wire_act reac_knock_wire_step(struct reac_knock_wire *w,
                                              const struct reac_hunt *hunt,
                                              uint64_t now_ns, int trunk_now);

/* A REAC frame was heard on this wire. It cancels the licence (libreac's rule: ANY frame).
 * While knocking, a frame that is EVIDENCE (a sighting whose settled role is a box or a
 * master, reac_disco, never a peer of unknown role) is the answer: the knock ends
 * (ACT_HUSH). Unknown-role FILLER does not end it, and that is the case that needs the knock
 * most: a box that has just come up floods broadcast FILLER and goes on flooding until it
 * has heard a master (libreac reac_fsm.c FSM_FLOOD_ANNOUNCE needs `have_master`), and only
 * then cold-connects. The FIRST frame after REAC_KNOCK_LISTEN_NS of silence resets the plan
 * and the backoff, and a resting knock opens at once: something just came up. Control
 * frames heard from the answer on are kept, up to REAC_KNOCK_WIRE_ANSWERS. On a trunk it is
 * a VLAN's frame with the tag stripped and changes nothing here. */
enum reac_knock_wire_act reac_knock_wire_heard(struct reac_knock_wire *w, uint64_t now_ns,
                                               const uint8_t *frame, size_t len,
                                               int evidence);

/* Send every frame of the knock whose slot has come, through `send`, and open the next
 * knock when its rest is over. Returns how many were sent; 0 when not knocking. `w->due_ns`
 * is then the next frame's time, a rest included. */
int reac_knock_wire_run(struct reac_knock_wire *w, uint64_t now_ns,
                        reac_knock_wire_send_fn send, void *ctx);

/* The kept answer was handed on; forget it. */
void reac_knock_wire_answer_taken(struct reac_knock_wire *w);

/* The period between the opening of unanswered knock n and of knock n+1: one cycle of the
 * master at this cadence, doubled n times, capped at REAC_KNOCK_WIRE_PERIOD_MAX_NS. */
uint64_t reac_knock_wire_period_ns(const struct reac_knock_wire *w, unsigned n);

/* THE FRAMES ONE CYCLE OF libreac's HUNTING MASTER STAMPS WITH A CONTROL BLOCK, counted by
 * stepping a fresh master through a cycle from the header, and the cycle's whole slot
 * count. Derived, never restated: a libreac that changes its cadence changes both. */
int reac_knock_wire_control_per_cycle(int fps);
int reac_knock_wire_cycle_slots(int fps);

const char *reac_knock_wire_state_name(enum reac_knock_wire_state s);
const char *reac_knock_content_name(enum reac_knock_content c);

/* THE KNOCK'S WAKE LADDER (#40, reac_wake.h). A box that has DROPPED answers no frame at all,
 * a knock included, and leaves that state on a PHY edge and on nothing else (reac_wake.h).
 * The knock's completed pushes feed the same ladder a pinned master's do. One spell per
 * knock spell (w->knocks), so its grace runs across the rests of a backed-off knock; the
 * frames that left are the spell's own (w->spell_sent) for the same reason, where a
 * per-second count would read the rests, and the ladder's own first step, as "nothing leaves
 * the host". */
struct reac_knock_ladder {
	struct reac_wake wake;
	uint64_t pushes0;          /* the knock's completed pushes when this spell opened      */
	uint64_t pushes_last;
	uint64_t frames0;          /* the frames this wire's socket had heard then            */
	unsigned knocks_seen;      /* which spell of the wire the counters belong to          */
};

/* One turn, once a second. `rx_frames` is every 0x8819 frame the wire's socket has heard,
 * `carrier`, `siblings_served` and `host_nic` as reac_wake_obs has them. `o` is filled with
 * what the ladder was shown, for the journal. A fresh spell or a fresh sniffer re-opens the
 * grace and keeps the edge count (reac_wake_reopen). */
enum reac_wake_act reac_knock_ladder_step(struct reac_knock_ladder *kl,
                                          const struct reac_knock_wire *w, uint64_t now_ns,
                                          uint64_t rx_frames, int carrier,
                                          int siblings_served, int host_nic,
                                          struct reac_wake_obs *o);

#endif /* REAC_KNOCK_WIRE_H */
