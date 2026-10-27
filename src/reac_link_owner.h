// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_link_owner — IS THIS INTERFACE THE HOST'S, OR A REAC WIRE AND NOTHING ELSE?
 *
 * THE DEFECT IT EXISTS AGAINST (#46), measured on the operator's desk 2026-10-26 19:19Z:
 * the knock on the trunk parent, a NIC that carries the desk's management address and is
 * the parent of every REAC VLAN, sent a stream cycle (8044 frames a second for 2.7 s) and
 * then its wake ladder took that NIC down for 1200 ms. The edge is a host network change:
 * every browser tab on the host aborts every request and every open stream, the console
 * page re-dials, NetworkManager re-runs its address setup. A build that ran all afternoon
 * made that edge every 17 s for four hours, and that is what froze the console UI.
 *
 * The wake ladder (reac_wake.h) is designed for a box that DROPPED and waits on a PHY
 * edge. That premise holds for a NIC that is a REAC segment and nothing else; it does not
 * hold for a NIC the host also uses, and nothing on the wire tells the two apart. The
 * kernel does. An interface is THE HOST'S when any of these is true of it:
 *
 *   - it carries an address of a scope wider than link (global or site, IPv4 or IPv6): the
 *     host reaches something through it. A bare REAC NIC carries only the kernel's fe80
 *     and NetworkManager's 169.254, both link scope, and those do not count;
 *   - a netdev is stacked on it (a VLAN, macvlan or ipvlan sub-interface, ours or the
 *     host's) or it is enslaved to a bridge or a bond: a box behind it is reached through
 *     its VLAN, never by cycling the parent;
 *   - a default route goes out of it, in any table.
 *
 * On the host's NIC the daemon makes no PHY edge (reac_wake REAC_WAKE_HOST_NIC) and sends
 * no stream knock (reac_knock_wire). On a bare NIC, the direct-cable case, nothing changes.
 * 2026-10-26-a-host-nic-is-never-bounced-or-streamed-at is the ruling.
 *
 * TWO HALVES. The verdict is PURE over three counts, so a test can pin every row; the
 * read is three rtnetlink dumps (addresses, links, routes), all unprivileged. A read that
 * fails is UNREAD, and UNREAD is the host's: an unreadable fact is never evidence that an
 * edge is safe (reac_wake's own carrier rule: -1 is not 0). */
#ifndef REAC_LINK_OWNER_H
#define REAC_LINK_OWNER_H

/* What the kernel said about one interface. */
struct reac_link_owner_facts {
	int addrs;           /* addresses on it of a scope wider than link             */
	int stacked;         /* netdevs stacked on it, plus 1 if it is itself enslaved  */
	int default_routes;  /* default routes (dst /0, unicast) out of it, any table   */
	int err;             /* 0, or -errno of the dump that failed                   */
};

enum reac_link_owner {
	REAC_LINK_OWNER_BARE = 0,  /* a wire of ours and nothing else                    */
	REAC_LINK_OWNER_ADDR,      /* the host's: it carries an address                  */
	REAC_LINK_OWNER_STACKED,   /* the host's: something is stacked on it             */
	REAC_LINK_OWNER_ROUTE,     /* the host's: a default route goes out of it         */
	REAC_LINK_OWNER_UNREAD,    /* rtnetlink could not be read: treated as the host's */
};

/* PURE. The first fact in the order above names the verdict; `err` makes it UNREAD. */
enum reac_link_owner reac_link_owner_verdict(const struct reac_link_owner_facts *f);

/* 1 for everything but BARE. */
int reac_link_owner_is_hosts(enum reac_link_owner v);

/* The verdict as the operator reads it in the journal and the roster. Never NULL. */
const char *reac_link_owner_text(enum reac_link_owner v);

/* Read `ifname` from rtnetlink and fill `f`; returns the verdict of what was read (UNREAD
 * with `f->err` set when a dump failed or the name is unknown). Unprivileged. */
enum reac_link_owner reac_link_owner_read(const char *ifname, struct reac_link_owner_facts *f);

#endif /* REAC_LINK_OWNER_H */
