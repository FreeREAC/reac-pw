// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock_wire — A WIRE NOBODY PINNED IS LISTENED TO, THEN KNOCKED ON, NEVER FLOODED.
 *
 * THE DEFECT IT EXISTS AGAINST (#31, #40). libreac's masterless licence (reac_knock.h) proves
 * that a wire which carried no REAC frame for 500 ms has no master on it, and until this module
 * the daemon answered that proof by taking the wire as MASTER: the whole stream, 4000 or 8000
 * frames a second, onto a wire on which nothing had ever been heard. On the untagged native
 * VLAN of a trunk that is ~96 Mbit/s of broadcast poured into two switches' management VLAN,
 * and a managed switch shut the port every 17 s for as long as the daemon ran (desk,
 * 2026-10-24). The journal said the assumption out loud: "NOTHING at all was heard on it ...
 * taking the wire as MASTER". Operator ruling 2026-10-25 (#40): a master never floods a wire
 * searching for a box; a box straight on a NIC stays supported, and the rule is the METHOD.
 *
 * WHAT A COLD BOX ANSWERS, which is what the knock has to be. Not a lone announce: one every
 * two seconds was measured on the rig with rx +0 for over a minute (libreac reac_knock.h,
 * "A COLD BOX DOES NOT ANSWER A LONE ANNOUNCE"). A linked silent box answers ONE COMPLETE
 * SCENE TRANSFER, FIRST + 341 MIDDLE + LAST, ~8.4 ms after the last chunk, and ignores an
 * interrupted one (reac_wake.h REAC_WAKE_MIN_PUSHES, the desk-arrival capture of 2026-09-14;
 * libreac tools/fake_box.c is built on the same capture). The transfer is carried in the
 * hunting master's own control cycle (libreac reac_master.c control_cadence: the header at
 * cycle_len - 5, the 341 chunks one per probe stride, the final chunk, one channel-map frame,
 * the cfea announce once a second), and every other slot of that cycle is an audio FILLER.
 *
 * SO THE KNOCK IS LIBREAC'S OWN MASTER WITH ITS FILLER SLOTS LEFT EMPTY. A `struct
 * reac_master` is stepped slot by slot at the wire's cadence exactly as the pacer steps it,
 * every frame it would stamp with a control block is sent at its slot's time, and every
 * FILLER is not sent at all. Same frames, same slots, same source address as the master that
 * follows: nothing here encodes a frame of its own. That is at most
 * REAC_KNOCK_WIRE_BUDGET_PER_CYCLE frames per cycle against the stream's whole cycle.
 *
 * THE FIRST FRAME HEARD ENDS IT, and the stream starts only after a box answered. The answer
 * is kept (REAC_KNOCK_WIRE_ANSWERS frames) because it is a fact about the push that produced
 * it: the box committed THIS scene and reports it once (fake_box answers one complete
 * transfer, then waits for the ENROLL group map). The master that is started on it must hear
 * that report or it waits in PROBING for one that never comes, so the caller hands the kept
 * frames to the new master before its pacer runs.
 *
 * A TRUNK PARENT IS NOT A SEGMENT (#39). It knocks untagged like any NIC until tagged REAC is
 * heard on one of its VLANs; from then on nothing is sent on it, the frames its socket hears
 * with their tags stripped are not answers, and it never becomes a segment.
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

/* THE CFEA ANNOUNCES A CYCLE CAN HOLD. The master announces once a second (reac_master.c:
 * `announce_tick >= fps`) and its hunting cycle is 10778 slots at 4000 fps, scaled by fps
 * (reac_master.c `cycle_len`), so 2.69 s at every rate: two or three announces. */
#define REAC_KNOCK_WIRE_ANNOUNCES_PER_CYCLE 3

/* THE KNOCK'S BUDGET, per cycle of the hunting master: the whole scene transfer, one
 * channel-map frame and the announces. 347 frames per ~2.7 s, about 129 a second, where
 * the stream is the cycle's every slot (10778 at 48 kHz, 21556 at 96 kHz). */
#define REAC_KNOCK_WIRE_BUDGET_PER_CYCLE \
	(REAC_SCENE_STEPS + 1 + REAC_KNOCK_WIRE_ANNOUNCES_PER_CYCLE)

/* A SLOT WHOSE TIME PASSED BY MORE THAN THIS IS NOT SENT LATE. CHOSEN: four probe strides
 * (500 chunks a second, reac_master.h REAC_M_BURST_PER_SEC). The main loop stalls for half a
 * second while it serves a segment (main.c hearing_hunt), and sending what fell due during
 * the stall back to back would bunch a cycle's frames into one burst. The master is still
 * stepped through those slots, so the cadence resumes in place; the push they belonged to is
 * interrupted, which a box ignores, and the next cycle's is whole. */
#define REAC_KNOCK_WIRE_LATE_NS (8ULL * 1000000ULL)

/* The CONTROL frames kept from the answer, in order: the box's declaration or its JOIN, and
 * what follows before the poll serves the master. FILLER is not kept. */
#define REAC_KNOCK_WIRE_ANSWERS 4

enum reac_knock_wire_state {
	REAC_KNOCK_WIRE_LISTENING = 0, /* inside the masterless observation; nothing sent      */
	REAC_KNOCK_WIRE_KNOCKING,      /* proven masterless, nothing heard: the knock is out   */
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
	uint64_t t0_ns;                /* when slot 0 of this knock fell                        */
	uint64_t slot;                 /* the next slot the master is stepped through           */
	uint64_t due_ns;               /* when the next frame this knock sends falls due (0 = none) */
	/* WHAT LEFT, for the journal, the roster and the test's fake wire. */
	uint64_t sent;                 /* frames handed to the wire and accepted                */
	uint64_t send_errors;          /* frames the wire refused                               */
	uint64_t late;                 /* frames whose slot passed by REAC_KNOCK_WIRE_LATE_NS   */
	uint64_t pushes;               /* complete transfers sent: a FIRST, then the LAST       */
	uint64_t cycle;                /* the cycle the counters below describe                 */
	uint64_t cycle_sent;           /* frames sent in that cycle                             */
	uint64_t cycle_max;            /* the most any one cycle carried                        */
	int head_sent;                 /* this transfer's FIRST left, so its LAST completes it  */
	int chunks_sent;               /* MIDDLEs of this transfer that left                    */
	unsigned knocks;               /* how many times this wire has been knocked on          */
	/* WHAT ANSWERED. */
	uint8_t answer[REAC_KNOCK_WIRE_ANSWERS][REAC_FRAME_BYTES];
	size_t answer_len[REAC_KNOCK_WIRE_ANSWERS];
	int n_answer;
	uint8_t answer_src[6];
};

/* A wire has just come up. `src` is the NIC's own address and `fps` the cadence the master
 * on it would run at; nothing is sent until the licence is granted. */
void reac_knock_wire_init(struct reac_knock_wire *w, const uint8_t src[6], int fps,
                          uint64_t now_ns);

/* One step of the poll. `hunt` is the wire's verdict-in-progress: a PINNED wire is never
 * knocked (a pin serves on link, reac_hunt.h). `trunk_now` is the topology tap's answer:
 * tagged REAC heard on this parent's VLANs, fresh. The masterless licence is NEVER handed to
 * the hunt: a wire is mastered on the evidence of a box heard on it, never on its silence. */
enum reac_knock_wire_act reac_knock_wire_step(struct reac_knock_wire *w,
                                              const struct reac_hunt *hunt,
                                              uint64_t now_ns, int trunk_now);

/* A REAC frame was heard on this wire. It cancels the licence (libreac's rule), and while
 * knocking it is the answer: the knock ends (ACT_HUSH). Its control frames, and those that
 * follow, are kept up to REAC_KNOCK_WIRE_ANSWERS. On a trunk it is a VLAN's frame with the
 * tag stripped and changes nothing here. */
enum reac_knock_wire_act reac_knock_wire_heard(struct reac_knock_wire *w, uint64_t now_ns,
                                               const uint8_t *frame, size_t len);

/* Send every frame of the knock whose slot has come, through `send`. Returns how many were
 * sent; 0 when not knocking. `w->due_ns` is then the next one's time. */
int reac_knock_wire_run(struct reac_knock_wire *w, uint64_t now_ns,
                        reac_knock_wire_send_fn send, void *ctx);

/* The kept answer was handed on; forget it. */
void reac_knock_wire_answer_taken(struct reac_knock_wire *w);

const char *reac_knock_wire_state_name(enum reac_knock_wire_state s);

#endif /* REAC_KNOCK_WIRE_H */
