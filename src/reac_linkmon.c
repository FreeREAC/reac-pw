// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_linkmon — see reac_linkmon.h for why this exists.

#include "reac_linkmon.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* The carrier bit in ifi_flags. Defined by <linux/if.h>, which cannot be included beside
 * <net/if.h> on every libc; the value is kernel ABI and cannot move. */
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

/* One datagram can carry many RTM_NEWLINK messages (a dump reply especially). Sized so a
 * whole-host dump arrives in one or two reads on any plausible rig; an over-long reply is
 * simply read again rather than lost. */
#define LINKMON_BUF 32768

/* ---- the pure core -------------------------------------------------------------------- */

void reac_linkmon_init(struct reac_linkmon *m, const char *ifname)
{
	memset(m, 0, sizeof *m);
	m->fd = -1;
	m->carrier = -1;
	m->acted = -1;
	/* A name that does not FIT is not an interface name. Refusing it here (rather than
	 * truncating) is what keeps the watch from silently attaching to a different NIC whose
	 * name happens to share the first 15 characters. */
	if (ifname && *ifname && strlen(ifname) < sizeof m->ifname)
		memcpy(m->ifname, ifname, strlen(ifname) + 1);
}

void reac_linkmon_observe(struct reac_linkmon *m, const char *ifname, int carrier)
{
	if (!m->ifname[0] || !ifname || strcmp(ifname, m->ifname) != 0)
		return;
	m->msgs++;
	if (carrier < 0)
		return;   /* UNKNOWN is not a verdict: the last known state stands */
	m->carrier = carrier ? 1 : 0;
}

enum reac_link_edge reac_linkmon_take(struct reac_linkmon *m)
{
	if (m->carrier < 0)
		return REAC_LINK_EDGE_NONE;          /* nothing known: nothing to report */
	if (m->carrier == m->acted)
		return REAC_LINK_EDGE_NONE;          /* settled where we left it: a flap that
		                                      * cancelled itself is not a transition */
	if (m->carrier == 1) {
		m->acted = 1;
		m->ups++;
		return REAC_LINK_EDGE_UP;
	}
	/* Carrier is down. If we never saw it UP, nothing went away — a master that starts
	 * beside an unplugged cable has not lost a peer, it never had one. */
	if (m->acted != 1) {
		m->acted = 0;
		return REAC_LINK_EDGE_NONE;
	}
	m->acted = 0;
	m->downs++;
	return REAC_LINK_EDGE_DOWN;
}

int reac_linkmon_carrier(const struct reac_linkmon *m)
{
	return m->carrier;
}

/* Read ONE netlink message: fill `name` with the interface it describes and return its
 * carrier (1/0/-1 UNKNOWN). Returns -2 when the message is not about a link at all. */
static int msg_link(const struct nlmsghdr *nh, char *name, size_t namesz)
{
	if (nh->nlmsg_type != RTM_NEWLINK && nh->nlmsg_type != RTM_DELLINK)
		return -2;
	if (nh->nlmsg_len < NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct ifinfomsg)))
		return -2;

	const char *body = (const char *)nh + NLMSG_HDRLEN;
	const struct ifinfomsg *ifi = (const struct ifinfomsg *)body;
	const char *a   = body + NLMSG_ALIGN(sizeof(struct ifinfomsg));
	const char *end = (const char *)nh + nh->nlmsg_len;

	name[0] = '\0';
	int carrier = -1;
	int have_carrier = 0;

	while ((size_t)(end - a) >= sizeof(struct rtattr)) {
		const struct rtattr *rta = (const struct rtattr *)a;
		if (rta->rta_len < sizeof(struct rtattr) ||
		    (size_t)rta->rta_len > (size_t)(end - a))
			break;                        /* truncated: drop, never guess */
		const char *v = a + RTA_LENGTH(0);
		size_t vlen = (size_t)rta->rta_len - RTA_LENGTH(0);
		if (rta->rta_type == IFLA_IFNAME && vlen > 0 && vlen <= namesz) {
			memcpy(name, v, vlen);
			name[vlen - 1] = '\0';        /* the kernel NUL-terminates; insist */
		} else if (rta->rta_type == IFLA_CARRIER && vlen >= 1) {
			carrier = *(const unsigned char *)v ? 1 : 0;
			have_carrier = 1;
		}
		a += RTA_ALIGN(rta->rta_len);
	}
	if (!name[0])
		return -2;                            /* nameless: not ours to judge */

	/* RTM_DELLINK is the NIC LEAVING — a different fault, owned by reac_rx's vanish path
	 * (tests/iface-vanish-exits.sh). Reporting it as "no carrier" would send the operator
	 * to look for a cable that has no socket to be out of. */
	if (nh->nlmsg_type == RTM_DELLINK)
		return -1;

	/* IFLA_CARRIER is the kernel's direct answer. IFF_LOWER_UP is the same bit reachable
	 * from flags alone, kept because it is present in EVERY RTM_NEWLINK and the attribute
	 * is not guaranteed to be. Both read 0 for an administratively-down interface — which
	 * is the state /sys/class/net/<if>/carrier answers EINVAL for, so the watch sees a
	 * transition the poll must report as UNKNOWN. That is the whole reason to watch. */
	if (have_carrier)
		return carrier;
	return (ifi->ifi_flags & IFF_LOWER_UP) ? 1 : 0;
}

/* feed(), plus the dump terminator the resync needs. */
static void feed_ex(struct reac_linkmon *m, const void *buf, size_t len, int *done)
{
	size_t off = 0;
	while (len - off >= sizeof(struct nlmsghdr)) {
		const struct nlmsghdr *nh = (const struct nlmsghdr *)((const char *)buf + off);
		size_t l = nh->nlmsg_len;
		if (l < sizeof(struct nlmsghdr) || l > len - off)
			break;                        /* truncated: drop the remainder */
		if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
			if (done)
				*done = 1;
		} else {
			char name[IFNAMSIZ];
			int c = msg_link(nh, name, sizeof name);
			if (c != -2)
				reac_linkmon_observe(m, name, c);
		}
		off += NLMSG_ALIGN(l);
	}
}

void reac_linkmon_feed(struct reac_linkmon *m, const void *buf, size_t len)
{
	feed_ex(m, buf, len, NULL);
}

/* ---- the netlink half ------------------------------------------------------------------ */

int reac_linkmon_fd(const struct reac_linkmon *m)
{
	return m->fd;
}

void reac_linkmon_close(struct reac_linkmon *m)
{
	if (m->fd >= 0)
		close(m->fd);
	m->fd = -1;
}

int reac_linkmon_resync(struct reac_linkmon *m)
{
	if (m->fd < 0)
		return -1;

	struct {
		struct nlmsghdr  nh;
		struct ifinfomsg ifi;
	} req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_GETLINK;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nh.nlmsg_seq = 1;
	req.ifi.ifi_family = AF_UNSPEC;
	if (send(m->fd, &req, req.nh.nlmsg_len, 0) < 0)
		return -1;

	/* Bounded: a dump that never terminates must not hold the main loop. Losing the seed
	 * leaves the carrier UNKNOWN, which is inert and honest — never "down". */
	char buf[LINKMON_BUF] __attribute__((aligned(8)));
	int done = 0;
	for (int i = 0; i < 64 && !done; i++) {
		struct pollfd p = { .fd = m->fd, .events = POLLIN };
		int pr = poll(&p, 1, 200);
		if (pr <= 0)
			break;
		ssize_t n = recv(m->fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n <= 0)
			break;
		feed_ex(m, buf, (size_t)n, &done);
	}
	return done ? 0 : -1;
}

int reac_linkmon_open(struct reac_linkmon *m, const char *ifname)
{
	reac_linkmon_init(m, ifname);
	if (!m->ifname[0])
		return -1;

	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof sa);
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;   /* every link add/remove/flag change on the host */
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		close(fd);
		return -1;
	}
	m->fd = fd;

	/* Seed from the kernel through the SAME parser every later edge uses, so a broken
	 * parse fails at startup rather than at the one moment a box is trying to enrol. */
	reac_linkmon_resync(m);
	/* The state AT OPEN is not a transition: a master that starts with the cable in has
	 * not just seen link-up, and must not re-establish itself before it has begun. */
	m->acted = m->carrier;
	return 0;
}

enum reac_link_edge reac_linkmon_drain(struct reac_linkmon *m)
{
	if (m->fd < 0)
		return REAC_LINK_EDGE_NONE;

	char buf[LINKMON_BUF] __attribute__((aligned(8)));
	for (;;) {
		ssize_t n = recv(m->fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n > 0) {
			feed_ex(m, buf, (size_t)n, NULL);
			continue;
		}
		if (n < 0 && errno == ENOBUFS) {
			/* The kernel dropped multicast we never saw. The cache is now a
			 * guess, and the guess that matters is the dangerous one: believing
			 * a link that bounced never left. Ask again instead. */
			m->overruns++;
			reac_linkmon_resync(m);
			continue;
		}
		break;   /* EAGAIN (drained), or an error worth no action */
	}
	return reac_linkmon_take(m);
}
