/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_linkmon — WATCH the cable, do not merely ask about it (issue #95).
 *
 * reac_link.h answers "is there a carrier RIGHT NOW" when something thinks to ask. That is a
 * POLL, and it closed half of #95: the PROBING watchdog stopped blaming the box for a cable
 * that was out. The other half is that nothing ever asks at the moment it matters. On the rig,
 * repeatedly, the kernel logged `Link status is: 0` then `1` for every box power-cycle and
 * reac-pw logged nothing and stayed in PROBING until the daemon was restarted by hand.
 *
 * WHY A MASTER MUST WATCH. A stagebox leaves BOOT for ANNOUNCE on PHY LINK-UP and on nothing
 * else — "a data gap does NOT" (reac-firmware-re REAC-PROTOCOL-FROM-SOURCE §10.2). Link-up is
 * therefore the ONE instant a box ever enrols on, and it is the instant our own FSM is least
 * likely to be ready: it may still hold a departed peer, a grant sweep sized to it, and an
 * ESTABLISHED badge for hardware that is not there. Watching lets the master meet that instant
 * with a clean establishment instead of a stale one.
 *
 * WHY NETLINK AND NOT A FASTER POLL. A poll turns a transient into a coin flip: a box that
 * bounces its PHY inside one poll interval is invisible, and shortening the interval to cover
 * it spends CPU forever to catch an event the kernel will simply hand us. RTM_NEWLINK is the
 * kernel's own announcement of exactly this transition, it costs nothing while the wire is
 * quiet, and it carries the interface NAME so a re-enumerated NIC (a new ifindex under the same
 * name) is followed rather than lost.
 *
 * WHAT IT IS NOT. It does not touch the FSM, the pacer or the RX feeder, and it knows nothing
 * about REAC. It reports EDGES on one named interface; the caller decides what an edge means.
 * That separation is the point: `reac_link` was split out of the pacer precisely so the TX
 * pacer and the RX feeder would not acquire a dependency on each other to ask about a cable,
 * and this module keeps that line (five test binaries link the pacer without reac_rx.c).
 *
 * THE UNKNOWN ARM IS LOAD-BEARING. Carrier is 1 / 0 / -1, and -1 is NEVER "down". An
 * unreadable interface, a socket that failed to open, an RTM_DELLINK (the NIC vanished — a
 * different fault, with its own handling in reac_rx) all leave the last KNOWN state standing
 * and emit no edge. A predicate that cannot see must not be read as a verdict; that is how a
 * probe which stopped working comes to look like a quiet wire.
 *
 * NOT RT-SAFE (a socket read, a possible dump). Drive it from a main loop, never from the
 * audio path or the pacer thread.
 */
#ifndef REAC_LINKMON_H
#define REAC_LINKMON_H

#include <net/if.h>   /* IFNAMSIZ */
#include <stddef.h>

/* What the caller is told. An edge is reported at most ONCE per real change: the settled
 * carrier is compared against the last state an edge was RETURNED for, so a flap that
 * resolves between two drains collapses to the one transition that actually happened.
 * A master must not re-establish once per bounce of a chattering PHY. */
enum reac_link_edge {
	REAC_LINK_EDGE_NONE = 0,
	REAC_LINK_EDGE_UP,     /* carrier RETURNED: the one event a box enrols on */
	REAC_LINK_EDGE_DOWN,   /* carrier LOST: the peer is gone, whatever we still believe */
};

struct reac_linkmon {
	char ifname[IFNAMSIZ];   /* the interface watched, by NAME (see the header comment) */
	int  fd;                 /* AF_NETLINK/NETLINK_ROUTE, RTNLGRP_LINK; -1 = not open */
	int  carrier;            /* last OBSERVED: 1 up, 0 down, -1 UNKNOWN */
	int  acted;              /* last state an edge was RETURNED for; -1 = none yet */
	unsigned long ups;       /* edges reported, for the operator's line */
	unsigned long downs;
	unsigned long msgs;      /* RTM_NEWLINK messages accepted for THIS interface. Zero
	                          * after a period the operator knows had a link change means
	                          * the watch is not working — an absence that can be told
	                          * apart from a quiet wire only because this is counted. */
	unsigned long overruns;  /* ENOBUFS: the kernel dropped multicast we never saw, so
	                          * the cached state is suspect and gets re-dumped */
};

/* Open the watch on `ifname` and seed the current carrier from the kernel (an RTM_GETLINK
 * dump through the same parser, so startup exercises the path every later edge uses).
 * Returns 0 on success, -1 if the socket could not be opened (the struct is left safe and
 * inert: fd -1, carrier UNKNOWN, every call below a no-op). A failure is NOT fatal to a
 * master — it is one diagnostic lost, not a wire — so callers report and continue. */
int reac_linkmon_open(struct reac_linkmon *m, const char *ifname);

/* The pollable descriptor, or -1. Register it for read-readiness on the caller's main loop. */
int reac_linkmon_fd(const struct reac_linkmon *m);

void reac_linkmon_close(struct reac_linkmon *m);

/* Read the socket dry and return the SETTLED edge, if any. Call on read-readiness; safe to
 * call at any time (it never blocks). On ENOBUFS the cached state is re-dumped rather than
 * trusted, because a missed multicast is exactly the case where believing the cache reports
 * a link that came back as one that never left. */
enum reac_link_edge reac_linkmon_drain(struct reac_linkmon *m);

/* Ask the kernel for the interface's CURRENT state (RTM_GETLINK dump) and fold the reply in
 * through the same parser. Used at open and after an overrun. Returns 0 if the dump was sent
 * and read, -1 otherwise (state untouched, still UNKNOWN-safe). */
int reac_linkmon_resync(struct reac_linkmon *m);

/* ---- the pure core, exercised directly by the unit test -------------------------------- *
 * Split out so the decision can be tested with no socket, no NIC and no privilege — the
 * netlink half is only a byte source for it. */

/* Initialise WITHOUT opening a socket: fd -1, carrier and acted UNKNOWN. */
void reac_linkmon_init(struct reac_linkmon *m, const char *ifname);

/* Fold in one observation of `ifname` at `carrier` (1/0/-1). Observations for any other
 * interface are ignored, and -1 leaves the last known state standing. */
void reac_linkmon_observe(struct reac_linkmon *m, const char *ifname, int carrier);

/* Parse one netlink datagram, folding every RTM_NEWLINK/RTM_DELLINK it carries for OUR
 * interface into the state above. A truncated or malformed buffer is dropped, never guessed. */
void reac_linkmon_feed(struct reac_linkmon *m, const void *buf, size_t len);

/* Take the settled edge (and mark it taken). NONE while the carrier is UNKNOWN or unchanged
 * since the last edge. A first observation of DOWN with nothing known before it is NOT an
 * edge: we never saw the link up, so nothing went away. */
enum reac_link_edge reac_linkmon_take(struct reac_linkmon *m);

/* The last observed carrier: 1 / 0 / -1 UNKNOWN. */
int reac_linkmon_carrier(const struct reac_linkmon *m);

#endif /* REAC_LINKMON_H */
