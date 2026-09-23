// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_wake — WHEN A MASTER MAY BREAK ITS OWN LINK TO WAKE A BOX THAT HAS DROPPED.
 *
 * THE DEFECT IT EXISTS AGAINST, measured on the operator's desk 2026-09-16 (the whole
 * window is docs/design/notes/2026-09-16-a-box-that-never-came-back.md): the desk went to
 * s2idle for 77 minutes with an S-1608 enrolled on `enp131s0`. On resume the daemon
 * re-took the wire as MASTER and drove it correctly for SEVENTY-THREE MINUTES across two
 * processes — about 1620 completed scene transfers, the NIC's own counter showing 8003
 * frames a second going out — for `rx_box_frames=0` and not one frame back. Every ten
 * seconds it told the operator "a box that is LINKED AND SILENT ... answers a COMPLETED
 * scene push, so do not bounce it yet". It never answered.
 *
 * WHY NO FRAME COULD HAVE WORKED, and why this is not a protocol gap to fill with a new
 * one: the box's own decompiled FSM (reac-firmware-re/REAC-PROTOCOL-FROM-SOURCE.md §10.2,
 * REAC-CONNECTION-FSM.md) reads `BOOT --> ANNOUNCE: PHY LINK-UP (the only establish
 * trigger; a data gap does NOT)` and `LINKED --> ANNOUNCE: PHY link-down/up`. A box that
 * has torn its master down leaves that state on ONE event, and it is not a packet. Our
 * push is byte-identical to a real desk's (libreac spec 2026-09-14 §2 compares them field
 * by field), so there is nothing left to send differently.
 *
 * The same live window carries the positive control: at 15:51:12, seventy-three minutes
 * into the silence, a freshly powered S-4000S on that wire answered the SAME push and was
 * ESTABLISHED in seven seconds. The master was never broken. The difference was a PHY edge.
 *
 * SO THE MASTER MAKES THE EDGE. The daemon already owns this interface administratively —
 * it installs and removes its etf qdisc over rtnetlink and mints its VLAN children — and
 * taking it down and up for a moment is the box's `PHY LINK-UP`.
 *
 * PURE: one clock, one observation, one verdict. No sockets, no netlink, no frames. main.c
 * turns ACT_BOUNCE into the two rtnetlink writes and feeds the result back as the next
 * observation. The ladder, the constants and every refusal are
 * docs/design/specs/2026-09-16-a-dropped-box-wakes-on-a-phy-edge.md §3-§5; this header does
 * not restate them, it instantiates them.
 */
#ifndef REAC_WAKE_H
#define REAC_WAKE_H

#include <stdint.h>

/* THE CHEAP RUNG IS COUNTED IN COMPLETED TRANSFERS, NOT IN SECONDS. A clock says nothing
 * about whether our own push ever finished, and an INTERRUPTED push is measured to produce
 * nothing at all — the desk-arrival capture's negative control is 338 middle chunks and a
 * LAST with no FIRST, which the box ignored (libreac spec 2026-09-14 §1). The box in that
 * same capture answered a whole one 8.355 ms after its last chunk, so three whole transfers
 * is three times a proven-sufficient exposure. */
#define REAC_WAKE_MIN_PUSHES 3u

/* The floor UNDER the push count, never a trigger on its own: three cycles is 8.1 s at
 * 8000 fps and 8.8 s at 3675 fps, so twelve seconds cannot be reached before the pushes
 * could have been, at any rate this daemon serves. */
#define REAC_WAKE_GRACE_NS (12ULL * 1000000000ULL)

/* How long the interface stays administratively down. CHOSEN, and said to be chosen: no
 * capture measures how long a link must be down for an S-1608's PHY to register it.
 * Longer than a 1 Gb autoneg cycle, short enough that the segment's gap is about a second. */
#define REAC_WAKE_DOWN_MS 1200u

/* After an edge, the box owes us a bounded broadcast flood (~1.36 s), its unicast
 * cold-connect retries and our own ~1.6 s ENROLL->grant dwell before it has failed to
 * answer. Twenty seconds leaves room for a slow autoneg on top of all three. */
#define REAC_WAKE_SETTLE_NS (20ULL * 1000000000ULL)

/* Two edges are enough for a box that is there. A third is a port flapping at an empty
 * socket, and a daemon that flaps forever is worse than one that waits and says so. */
#define REAC_WAKE_MAX_BOUNCES 2u

/* What the caller must DO this step. */
enum reac_wake_act {
	REAC_WAKE_ACT_NONE = 0,  /* nothing to do; `refusal` says which fact stopped it */
	REAC_WAKE_ACT_BOUNCE,    /* take this master's own interface down and up, once  */
	REAC_WAKE_ACT_EXHAUSTED, /* returned EXACTLY ONCE: the ladder is spent, say so   */
};

/* Why a step did nothing. Every one of these is a FACT about the wire or about us, not a
 * delay — the spec's §4 table, one row each. */
enum reac_wake_refusal {
	REAC_WAKE_OK = 0,            /* nothing refused it (an act was returned)        */
	REAC_WAKE_NOT_PROBING,       /* granting or established: a bounce breaks what works */
	REAC_WAKE_NO_CARRIER,        /* no link to break; only the cable fixes this     */
	REAC_WAKE_CARRIER_UNKNOWN,   /* -1 is not 0; an unreadable probe is not evidence */
	REAC_WAKE_BOX_IS_TALKING,    /* rx_box_frames > 0: the far end is alive         */
	REAC_WAKE_NOTHING_SENT,      /* our own frames are not leaving the host          */
	REAC_WAKE_PUSH_NOT_PROVEN,   /* the cheap rung has not been played to the end   */
	REAC_WAKE_SETTLING,          /* inside the settle window of the last edge       */
	REAC_WAKE_SIBLING_SERVED,    /* this device carries other served segments       */
	REAC_WAKE_SPENT,             /* the ladder is spent; only the operator can act  */
};

/* What the daemon knows about this segment at this step. All four are read by the caller
 * from what it already has: the master FSM's mirrored state, the pacer's rx counter, the
 * master's completed-transfer count and the link's carrier. */
struct reac_wake_obs {
	int      probing;          /* the master FSM is in PROBING                      */
	int      carrier;          /* 1 up, 0 down, -1 UNKNOWN (never treated as down)  */
	uint64_t rx_box_frames;    /* frames received from any box, ever, this pacer    */
	uint64_t scene_pushes;     /* scene transfers COMPLETED since PROBING opened    */
	int      siblings_served;  /* other segments served over this same device       */
	/* FRAMES THAT LEFT THE HOST since the last observation — sendto() succeeded, not
	 * merely attempted. A "completed" push is counted by the master when it has HANDED
	 * its chunks to the pacer; whether the kernel then put them on the wire is this
	 * number. Desk 2026-09-23 13:43: an etf root qdisc under a thread-backend pacer
	 * refused every frame (tx=0, tx_errors=8000/s, 3.46 M by the end), the master
	 * counted 145 "COMPLETED" pushes over a wire that carried none of them, and the
	 * ladder bounced a port twice and gave up on a box that had never heard us. Zero
	 * here is a fact about US, and it refuses the edge before any fact about the box. */
	uint64_t tx_frames;
};

struct reac_wake {
	uint64_t opened_ns;        /* when this PROBING spell began                     */
	uint64_t last_bounce_ns;   /* when the last edge was made (0 = none yet)        */
	unsigned bounces;          /* edges made on this segment                        */
	int      spent_said;       /* ACT_EXHAUSTED has been returned; never twice      */
	enum reac_wake_refusal refusal;  /* why the last step did nothing               */
	enum reac_wake_refusal said;     /* the refusal last handed to the journal      */
};

/* Open on a segment whose master has just entered PROBING. */
void reac_wake_init(struct reac_wake *w, uint64_t now_ns);

/* The master left PROBING (granted, established, or the segment was re-served). The push
 * grace restarts from here; the BOUNCE COUNT DOES NOT, because a box that keeps dropping
 * back must not be flapped once per drop. */
void reac_wake_reopen(struct reac_wake *w, uint64_t now_ns);

/* Advance the clock against one observation. Returns ACT_BOUNCE at most
 * REAC_WAKE_MAX_BOUNCES times and ACT_EXHAUSTED at most once; `w->refusal` carries the fact
 * behind every ACT_NONE. */
enum reac_wake_act reac_wake_step(struct reac_wake *w, uint64_t now_ns,
                                  const struct reac_wake_obs *o);

/* SAY IT ONCE PER CHANGE OF FACT. After an ACT_NONE, returns 1 exactly once for each
 * NEW refusal — the caller prints reac_wake_refusal_text(w->refusal) — and 0 while the
 * same fact keeps refusing. Spec §4: "the daemon says it once". Until 2026-09-23 main.c
 * printed nothing at all for ACT_NONE, so a ladder that never acted was indistinguishable
 * from one that had never been asked; a reopen (the box enrolled and dropped) resets
 * this, because the refusal that follows is about a new spell. OK is never said. */
int reac_wake_refusal_to_say(struct reac_wake *w);

/* The refusal, as the operator reads it in the journal. Never NULL. */
const char *reac_wake_refusal_text(enum reac_wake_refusal r);

#endif /* REAC_WAKE_H */
