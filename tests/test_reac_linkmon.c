// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * reac_linkmon — the WATCH half of issue #95.
 *
 * reac_link's poll answers "is there a cable" when asked; this notices the ANSWER CHANGING,
 * which is the half that matters: a stagebox leaves BOOT for ANNOUNCE on PHY LINK-UP and on
 * nothing else (reac-firmware-re REAC-PROTOCOL-FROM-SOURCE §10.2), so the master has exactly
 * one instant to be ready and no way to know it arrived.
 *
 * THE POSITIVE CONTROL RUNS FIRST and against the REAL KERNEL. Every other case below feeds
 * bytes this file wrote, and a parser that understood none of them would pass all of them by
 * doing nothing. So the first thing asserted is that an RTM_GETLINK dump of the live host,
 * through the same parser, finds `lo` and reports it carrying. Absence is only evidence after
 * presence has been demonstrated.
 *
 * THE UNKNOWN ARM IS THE ONE TO BREAK. -1 is never "down": an RTM_DELLINK, a foreign
 * interface, a truncated datagram and a name too long to BE a name all leave the last known
 * state standing and emit no edge.
 *
 * NAMES HERE ARE <= 15 CHARS ON PURPOSE. IFNAMSIZ is 16, and a longer name is refused by the
 * length guard before it reaches anything — a test that used one would bounce off the guard
 * and prove nothing about the code it named. The guard gets its own case, deliberately.
 */
#include "reac_linkmon.h"

#include <stdio.h>
#include <string.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define IF_A "reac-test0"      /* the watched interface */
#define IF_B "reac-test1"      /* somebody else's */

/* Build one RTM_NEWLINK/RTM_DELLINK exactly as the kernel lays it out: header, ifinfomsg,
 * then IFLA_IFNAME and (optionally) IFLA_CARRIER. `carrier_attr` < 0 omits the attribute so
 * the IFF_LOWER_UP fallback is what gets exercised. Returns the bytes written. */
static size_t build_link(void *buf, int type, const char *ifname, int lower_up,
                         int carrier_attr)
{
	char *p = buf;
	struct nlmsghdr *nh = (struct nlmsghdr *)p;
	memset(nh, 0, NLMSG_HDRLEN);
	nh->nlmsg_type = (unsigned short)type;
	nh->nlmsg_flags = 0;

	struct ifinfomsg *ifi = (struct ifinfomsg *)(p + NLMSG_HDRLEN);
	memset(ifi, 0, sizeof *ifi);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = 7;
	ifi->ifi_flags = IFF_UP | (lower_up ? IFF_LOWER_UP : 0u);

	size_t off = NLMSG_HDRLEN + NLMSG_ALIGN(sizeof *ifi);

	size_t nlen = strlen(ifname) + 1;
	struct rtattr *rta = (struct rtattr *)(p + off);
	rta->rta_type = IFLA_IFNAME;
	rta->rta_len = (unsigned short)RTA_LENGTH(nlen);
	memcpy(p + off + RTA_LENGTH(0), ifname, nlen);
	off += RTA_ALIGN(rta->rta_len);

	if (carrier_attr >= 0) {
		struct rtattr *rc = (struct rtattr *)(p + off);
		rc->rta_type = IFLA_CARRIER;
		rc->rta_len = (unsigned short)RTA_LENGTH(1);
		*(unsigned char *)(p + off + RTA_LENGTH(0)) = (unsigned char)carrier_attr;
		off += RTA_ALIGN(rc->rta_len);
	}

	nh->nlmsg_len = (unsigned int)off;
	return off;
}

/* Feed one synthesised message. */
static void feed_one(struct reac_linkmon *m, int type, const char *ifname, int lower_up,
                     int carrier_attr)
{
	char buf[512] __attribute__((aligned(8)));
	size_t n = build_link(buf, type, ifname, lower_up, carrier_attr);
	reac_linkmon_feed(m, buf, n);
}

/* ---- the positive control: the REAL kernel, the REAL parser ---------------------------- */

static int live_dump_sees_loopback(void)
{
	struct reac_linkmon m;
	if (reac_linkmon_open(&m, "lo") != 0) {
		/* No netlink at all (a sandbox with AF_NETLINK denied). Say so LOUDLY: this is
		 * the arm that proves the parser works, and losing it silently would let every
		 * synthetic case below pass over a parser that reads nothing. */
		fprintf(stderr, "SKIP: AF_NETLINK unavailable — the live positive control could "
		        "not run, so the parser is UNPROVEN against real kernel bytes\n");
		return 77;
	}
	/* `lo` is up and carrying on every host this can run on. Anything else means the dump
	 * was not read, the name attribute was not found, or the carrier was not decoded. */
	CHK(reac_linkmon_carrier(&m) == 1);
	CHK(m.msgs > 0);
	CHK(reac_linkmon_fd(&m) >= 0);
	/* Opening is not an EDGE: a master that starts with the cable in has not just seen
	 * link-up and must not re-establish itself before it has begun. */
	CHK(reac_linkmon_drain(&m) == REAC_LINK_EDGE_NONE);
	reac_linkmon_close(&m);
	CHK(reac_linkmon_fd(&m) == -1);
	return 0;
}

int main(void)
{
	int skipped = live_dump_sees_loopback();

	struct reac_linkmon m;

	/* (1) The edge that matters: carrier RETURNS. */
	reac_linkmon_init(&m, IF_A);
	CHK(reac_linkmon_carrier(&m) == -1);          /* nothing known yet */
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_carrier(&m) == 1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);
	CHK(m.ups == 1);

	/* (2) Reported ONCE. The kernel repeats RTM_NEWLINK for changes that have nothing to
	 * do with carrier (a MAC change, promiscuous mode, an address); re-establishing on
	 * each would rebuild the segment for reasons the box never saw. */
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	CHK(m.ups == 1);

	/* (3) Carrier LOST — the drop the daemon never noticed (the issue's own repro). */
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_DOWN);
	CHK(m.downs == 1);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);

	/* (4) A FLAP THAT CANCELS ITSELF IS NOT A TRANSITION. Carrier is up; it bounces down
	 * and back between two drains. The wire ended where it started, so re-establishing
	 * would drop a box mid-audio for an event it survived. */
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	/* ...but a flap that SETTLES DOWN is one drop, not three. */
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_DOWN);
	CHK(m.downs == 2);

	/* (5) ANOTHER NIC'S LINK IS NOT OURS. The socket is a host-wide multicast group: every
	 * interface on the machine arrives here, and a rig runs two REAC segments plus a
	 * management NIC. Acting on a neighbour's cable would re-establish a healthy segment
	 * every time somebody plugged in a laptop. */
	reac_linkmon_init(&m, IF_A);
	feed_one(&m, RTM_NEWLINK, IF_B, 1, -1);
	CHK(m.msgs == 0);
	CHK(reac_linkmon_carrier(&m) == -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);

	/* (6) RTM_DELLINK IS NOT "NO CARRIER". The NIC LEFT — a different fault, owned by
	 * reac_rx's vanish path. Reporting it as a dead cable sends the operator to look for a
	 * socket that is no longer there; the last known state stands and no edge fires. */
	reac_linkmon_init(&m, IF_A);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);
	feed_one(&m, RTM_DELLINK, IF_A, 0, 0);
	CHK(m.msgs == 2);                              /* we SAW it */
	CHK(reac_linkmon_carrier(&m) == 1);            /* and did not read it as down */
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	CHK(m.downs == 0);

	/* (7) A FIRST-EVER DOWN IS NOT A LOSS. A master started beside an unplugged cable never
	 * had a peer; announcing one gone would be a fault report for a state that never was.
	 * The UP that follows is still an edge — that is the one the box enrols on. */
	reac_linkmon_init(&m, IF_A);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	CHK(m.downs == 0);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);

	/* (8) IFLA_CARRIER OUTRANKS IFF_LOWER_UP. The attribute is the kernel's direct answer;
	 * the flag bit is the fallback for messages that omit it. A message carrying both must
	 * be read by the attribute, or a disagreement resolves the wrong way silently. */
	reac_linkmon_init(&m, IF_A);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, 0);         /* LOWER_UP set, CARRIER says 0 */
	CHK(reac_linkmon_carrier(&m) == 0);
	feed_one(&m, RTM_NEWLINK, IF_A, 0, 1);         /* LOWER_UP clear, CARRIER says 1 */
	CHK(reac_linkmon_carrier(&m) == 1);

	/* (9) A TRUNCATED DATAGRAM IS DROPPED, NOT GUESSED. Half a message that read as a
	 * carrier value would be an invented verdict, and the invented one is always cheap to
	 * believe. State must not move. */
	reac_linkmon_init(&m, IF_A);
	feed_one(&m, RTM_NEWLINK, IF_A, 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);
	{
		char buf[512] __attribute__((aligned(8)));
		size_t n = build_link(buf, RTM_NEWLINK, IF_A, 0, -1);
		reac_linkmon_feed(&m, buf, n - 4);      /* the tail never arrived */
		CHK(reac_linkmon_carrier(&m) == 1);
		CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
		/* A length field claiming more than the buffer holds: same answer. */
		n = build_link(buf, RTM_NEWLINK, IF_A, 0, -1);
		((struct nlmsghdr *)buf)->nlmsg_len = (unsigned int)(n + 64);
		reac_linkmon_feed(&m, buf, n);
		CHK(reac_linkmon_carrier(&m) == 1);
		/* An attribute claiming more than the message holds: the name is never read,
		 * so the message is nameless and not ours to judge. */
		n = build_link(buf, RTM_NEWLINK, IF_A, 0, -1);
		((struct rtattr *)(buf + NLMSG_HDRLEN +
		                   NLMSG_ALIGN(sizeof(struct ifinfomsg))))->rta_len = 0xfff0;
		reac_linkmon_feed(&m, buf, n);
		CHK(reac_linkmon_carrier(&m) == 1);
		CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
		/* An empty datagram. */
		reac_linkmon_feed(&m, buf, 0);
		CHK(reac_linkmon_carrier(&m) == 1);
	}

	/* (10) THE LENGTH GUARD, REACHED ON PURPOSE. IFNAMSIZ is 16. A name that does not fit
	 * is refused whole rather than truncated — truncating would attach the watch to a
	 * DIFFERENT NIC sharing the first 15 characters, and the watch would then be perfectly
	 * healthy about the wrong cable. The 15-char case beside it proves the boundary is a
	 * boundary and not a wall. */
	reac_linkmon_init(&m, "reacpw-test-does-not-exist-9182");   /* 31 chars */
	CHK(m.ifname[0] == '\0');
	feed_one(&m, RTM_NEWLINK, "reacpw-test-does-not-exist-9182", 1, -1);
	CHK(m.msgs == 0);
	CHK(reac_linkmon_carrier(&m) == -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_NONE);
	CHK(reac_linkmon_open(&m, "reacpw-test-does-not-exist-9182") == -1);
	CHK(reac_linkmon_fd(&m) == -1);
	CHK(reac_linkmon_drain(&m) == REAC_LINK_EDGE_NONE);   /* inert, not crashing */

	reac_linkmon_init(&m, "reacpw-testif15");                   /* 15 chars: legal */
	CHK(strcmp(m.ifname, "reacpw-testif15") == 0);
	feed_one(&m, RTM_NEWLINK, "reacpw-testif15", 1, -1);
	CHK(reac_linkmon_take(&m) == REAC_LINK_EDGE_UP);

	/* (11) NULL and empty are names too, and both are refused. */
	reac_linkmon_init(&m, NULL);
	CHK(m.ifname[0] == '\0');
	CHK(reac_linkmon_open(&m, "") == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	if (skipped == 77)
		return 77;
	printf("OK: reac_linkmon WATCHES the cable — a live RTM_GETLINK dump reads lo as "
	       "carrying, carrier return and loss are one edge each, a self-cancelling flap is "
	       "none, and every unreadable case answers UNKNOWN rather than down\n");
	return 0;
}
