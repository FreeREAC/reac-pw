/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_served_mark — THE DAEMON SAYS WHICH INTERFACES ARE ITS OWN, ON THE INTERFACE.
 *
 * THE DEFECT IT EXISTS AGAINST, measured on the operator's desk 2026-10-11: the REAC NIC
 * was managed by NetworkManager. With no box answering, the wake ladder (reac_wake.h)
 * takes the link down and up; NetworkManager answers every edge by deleting and re-adding
 * the link-local IPv4/IPv6 addresses it put there, and every address change makes a web
 * browser abort ALL of its sockets, the console's on 127.0.0.1 included. A REAC wire is
 * raw Layer 2 and wants no address and no network manager at all.
 *
 * WHO CAN SAY SO. Only this daemon knows which interfaces are REAC segments: it hears
 * them. But it is a USER service, so it cannot write NetworkManager's configuration, and
 * NetworkManager refuses a session-less caller its device controls. What it can do, with
 * the CAP_NET_ADMIN it already holds to bounce the link, is set the interface ALIAS. So
 * the fact is written where it is true — on the netdev — and the root side
 * (packaging/reac-pw-nm-unmanaged, run as a system service) reads every alias that starts
 * with REAC_ALIAS_PREFIX and generates NetworkManager's drop-in from them. Netdevs the
 * daemon mints already carry `reac-pw:minted` (libreac's REAC_VLAN_ALIAS) and are found
 * by the same prefix.
 *
 * WHAT IS MARKED. Every interface a segment comes up on, except:
 *   - one whose alias is set to anything else: the host named it, the name stays;
 *   - a VLAN netdev with no alias: the host made it (an adopted sub-interface, trunk spec
 *     §4d), often through a NetworkManager profile that would stop creating it at boot if
 *     NetworkManager were told to leave it alone.
 * A clean exit removes exactly the marks this daemon wrote and that are still its own. */
#ifndef REAC_SERVED_MARK_H
#define REAC_SERVED_MARK_H

#include <stdio.h>

#define REAC_ALIAS_PREFIX  "reac-pw:"
#define REAC_SERVED_ALIAS  "reac-pw:served"
#define REAC_SERVED_MAX    32

enum reac_served_verdict {
	REAC_SERVED_MARK = 0,     /* no alias, not a host VLAN: write REAC_SERVED_ALIAS   */
	REAC_SERVED_OURS,         /* already carries a reac-pw: alias: nothing to write   */
	REAC_SERVED_LEAVE_VLAN,   /* a VLAN the host made: left exactly as found          */
	REAC_SERVED_LEAVE_HOST,   /* the host set its own alias: left exactly as found    */
};

/* PURE. `kind` is the link kind ("" for a physical NIC), `alias` its current alias. */
enum reac_served_verdict reac_served_decide(const char *kind, const char *alias);

/* What this daemon wrote, so the exit removes exactly that. Zeroed = nothing. */
struct reac_served_marks {
	int  n;
	char name[REAC_SERVED_MAX][16];
};

/* Mark `ifname` as served, saying on stderr what was done and why when anything other
 * than a plain mark happened. Returns the verdict, or -errno when the alias could not be
 * read or written (the segment is served either way; only the marker is missing). */
int reac_served_mark(struct reac_served_marks *m, const char *ifname);

/* WHERE THOSE LINES GO. The daemon leaves it at standard error, and NULL puts it back
 * there. A caller that may write nothing — the fuzz harness of this policy, which leaves
 * the output of a run to its driver — points it at a stream of its own. What the policy
 * decides, writes and returns is the same either way. */
void reac_served_say_to(FILE *out);

/* Remove every mark in `m` whose alias is still REAC_SERVED_ALIAS, and clear `m`. An
 * interface that is gone, or whose alias somebody changed since, is left alone. */
void reac_served_unmark_all(struct reac_served_marks *m);

#endif
