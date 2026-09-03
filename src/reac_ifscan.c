/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_ifscan — the interface table and its transitions; see reac_ifscan.h.
 */
#include "reac_ifscan.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <net/if_arp.h>       /* ARPHRD_ETHER */
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* The carrier bit in ifi_flags. Defined by <linux/if.h>, which cannot be included beside
 * <net/if.h>; the value is ABI. Same for the loopback bit, which <net/if.h> does define. */
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif

/* One datagram can carry many messages (a dump reply especially). */
#define IFSCAN_BUF 32768

const char *reac_ifscan_state_name(enum reac_ifscan_state s)
{
	switch (s) {
	case REAC_IFSCAN_ABSENT:  return "absent";
	case REAC_IFSCAN_DOWN:    return "down";
	case REAC_IFSCAN_LINKED:  return "linked";
	case REAC_IFSCAN_SEGMENT: return "segment";
	case REAC_IFSCAN_HOLD:    return "hold";
	}
	return "?";
}

const char *reac_ifscan_verb_name(enum reac_ifscan_verb v)
{
	switch (v) {
	case REAC_IFSCAN_NONE:     return "none";
	case REAC_IFSCAN_LISTEN:   return "listen";
	case REAC_IFSCAN_UNLISTEN: return "unlisten";
	case REAC_IFSCAN_SERVE:    return "serve";
	case REAC_IFSCAN_DROP:     return "drop";
	case REAC_IFSCAN_KEPT:     return "kept";
	}
	return "?";
}

void reac_ifscan_init(struct reac_ifscan *s)
{
	memset(s, 0, sizeof *s);
	s->fd = -1;
}

static void emit(struct reac_ifscan *s, enum reac_ifscan_verb v, const char *name)
{
	int next = (s->ev_tail + 1) % REAC_IFSCAN_EVENTS;
	if (next == s->ev_head) {
		s->dropped_ev++;
		return;
	}
	s->ev[s->ev_tail].verb = v;
	snprintf(s->ev[s->ev_tail].name, IFNAMSIZ, "%s", name);
	s->ev_tail = next;
}

int reac_ifscan_next(struct reac_ifscan *s, struct reac_ifscan_event *out)
{
	if (s->ev_head == s->ev_tail)
		return 0;
	*out = s->ev[s->ev_head];
	s->ev_head = (s->ev_head + 1) % REAC_IFSCAN_EVENTS;
	return 1;
}

static struct reac_ifscan_entry *find(struct reac_ifscan *s, const char *name)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (s->ifs[i].state != REAC_IFSCAN_ABSENT && strcmp(s->ifs[i].name, name) == 0)
			return &s->ifs[i];
	return NULL;
}

const struct reac_ifscan_entry *reac_ifscan_find(const struct reac_ifscan *s, const char *name)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (s->ifs[i].state != REAC_IFSCAN_ABSENT && strcmp(s->ifs[i].name, name) == 0)
			return &s->ifs[i];
	return NULL;
}

int reac_ifscan_count(const struct reac_ifscan *s, enum reac_ifscan_state state)
{
	int n = 0;
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (s->ifs[i].state == state)
			n++;
	return n;
}

/* Leave the table, undoing whatever the caller holds for this entry. */
static void leave(struct reac_ifscan *s, struct reac_ifscan_entry *e)
{
	switch (e->state) {
	case REAC_IFSCAN_LINKED:
		emit(s, REAC_IFSCAN_UNLISTEN, e->name);
		break;
	case REAC_IFSCAN_SEGMENT:
	case REAC_IFSCAN_HOLD:
		emit(s, REAC_IFSCAN_DROP, e->name);
		break;
	case REAC_IFSCAN_DOWN:
	case REAC_IFSCAN_ABSENT:
		break;
	}
	memset(e, 0, sizeof *e);
}

void reac_ifscan_observe(struct reac_ifscan *s, const char *name, int ifindex, int ether,
                         int lower_up, uint64_t now_ns)
{
	(void)now_ns;
	if (!name || !name[0] || strlen(name) >= IFNAMSIZ)
		return;
	struct reac_ifscan_entry *e = find(s, name);
	if (!ether) {
		/* Not a segment candidate. If it once was (a NIC turned into a bridge
		 * port would still be ether; this is belt and braces), it leaves. */
		if (e)
			leave(s, e);
		return;
	}
	if (e && e->ifindex != ifindex) {
		/* The same name, a new index: the 2026-08-29 fault. Gone, then new. */
		leave(s, e);
		e = NULL;
	}
	if (!e) {
		for (int i = 0; i < REAC_IFSCAN_MAX; i++) {
			if (s->ifs[i].state == REAC_IFSCAN_ABSENT) {
				e = &s->ifs[i];
				break;
			}
		}
		if (!e) {
			s->unbounded++;
			return;
		}
		memset(e, 0, sizeof *e);
		snprintf(e->name, IFNAMSIZ, "%s", name);
		e->ifindex = ifindex;
		e->state = REAC_IFSCAN_DOWN;
	}
	switch (e->state) {
	case REAC_IFSCAN_DOWN:
		if (lower_up) {
			e->state = REAC_IFSCAN_LINKED;
			e->retry_after_ns = 0;
			emit(s, REAC_IFSCAN_LISTEN, e->name);
		}
		break;
	case REAC_IFSCAN_LINKED:
		if (!lower_up) {
			e->state = REAC_IFSCAN_DOWN;
			emit(s, REAC_IFSCAN_UNLISTEN, e->name);
		}
		break;
	case REAC_IFSCAN_SEGMENT:
		if (!lower_up) {
			e->state = REAC_IFSCAN_HOLD;
			e->hold_until_ns = now_ns + REAC_IFSCAN_DOWN_HOLD_NS;
		}
		break;
	case REAC_IFSCAN_HOLD:
		if (lower_up) {
			e->state = REAC_IFSCAN_SEGMENT;
			e->hold_until_ns = 0;
			e->flaps++;
			emit(s, REAC_IFSCAN_KEPT, e->name);
		}
		break;
	case REAC_IFSCAN_ABSENT:
		break;
	}
}

void reac_ifscan_gone(struct reac_ifscan *s, const char *name, int ifindex, uint64_t now_ns)
{
	(void)now_ns;
	if (!name || !name[0])
		return;
	struct reac_ifscan_entry *e = find(s, name);
	if (!e)
		return;
	if (ifindex != 0 && e->ifindex != ifindex)
		return;   /* a stale message about an index we already replaced */
	leave(s, e);
}

void reac_ifscan_heard(struct reac_ifscan *s, const char *name, uint64_t now_ns)
{
	struct reac_ifscan_entry *e = find(s, name);
	if (!e || e->state != REAC_IFSCAN_LINKED)
		return;
	if (e->retry_after_ns != 0 && now_ns < e->retry_after_ns)
		return;
	e->state = REAC_IFSCAN_SEGMENT;
	e->retry_after_ns = 0;
	emit(s, REAC_IFSCAN_SERVE, e->name);
}

void reac_ifscan_serve_failed(struct reac_ifscan *s, const char *name, uint64_t now_ns)
{
	struct reac_ifscan_entry *e = find(s, name);
	if (!e || (e->state != REAC_IFSCAN_SEGMENT && e->state != REAC_IFSCAN_HOLD))
		return;
	int had_link = e->state == REAC_IFSCAN_SEGMENT;
	e->state = had_link ? REAC_IFSCAN_LINKED : REAC_IFSCAN_DOWN;
	e->hold_until_ns = 0;
	e->retry_after_ns = now_ns + REAC_IFSCAN_RETRY_NS;
	if (had_link)
		emit(s, REAC_IFSCAN_LISTEN, e->name);
}

void reac_ifscan_tick(struct reac_ifscan *s, uint64_t now_ns)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++) {
		struct reac_ifscan_entry *e = &s->ifs[i];
		if (e->state != REAC_IFSCAN_HOLD || now_ns < e->hold_until_ns)
			continue;
		e->state = REAC_IFSCAN_DOWN;
		e->hold_until_ns = 0;
		emit(s, REAC_IFSCAN_DROP, e->name);
	}
}

/* ---- wireless exclusion -------------------------------------------------------------------- */

int reac_ifscan_is_wireless(const char *root, const char *ifname)
{
	if (!ifname || !ifname[0])
		return 0;
	if (!root || !root[0])
		root = "/sys/class/net";

	char path[512];
	struct stat st;
	int n = snprintf(path, sizeof path, "%s/%s/wireless", root, ifname);
	if (n > 0 && (size_t)n < sizeof path && stat(path, &st) == 0)
		return 1;
	n = snprintf(path, sizeof path, "%s/%s/phy80211", root, ifname);
	if (n > 0 && (size_t)n < sizeof path && stat(path, &st) == 0)
		return 1;
	return 0;
}

int reac_ifscan_wireless_allowed(const char *allowlist, const char *ifname)
{
	if (!allowlist || !allowlist[0] || !ifname || !ifname[0])
		return 0;
	if (strcmp(allowlist, "*") == 0)
		return 1;
	size_t iflen = strlen(ifname);
	const char *p = allowlist;
	while (*p) {
		const char *comma = strchr(p, ',');
		size_t seglen = comma ? (size_t)(comma - p) : strlen(p);
		if (seglen == iflen && strncmp(p, ifname, seglen) == 0)
			return 1;
		p += seglen;
		if (*p == ',')
			p++;
	}
	return 0;
}

/* ---- netlink ----------------------------------------------------------------------------- */

/* One RTM_NEWLINK/RTM_DELLINK into the table. Anything else is not ours. */
static void msg_link(struct reac_ifscan *s, const struct nlmsghdr *nh, uint64_t now_ns)
{
	if (nh->nlmsg_type != RTM_NEWLINK && nh->nlmsg_type != RTM_DELLINK)
		return;
	if (nh->nlmsg_len < NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct ifinfomsg)))
		return;

	const char *body = (const char *)nh + NLMSG_HDRLEN;
	const struct ifinfomsg *ifi = (const struct ifinfomsg *)body;
	const char *a   = body + NLMSG_ALIGN(sizeof(struct ifinfomsg));
	const char *end = (const char *)nh + nh->nlmsg_len;

	char name[IFNAMSIZ];
	name[0] = '\0';
	while ((size_t)(end - a) >= sizeof(struct rtattr)) {
		const struct rtattr *rta = (const struct rtattr *)a;
		if (rta->rta_len < sizeof(struct rtattr) ||
		    (size_t)rta->rta_len > (size_t)(end - a))
			break;                        /* truncated: drop, never guess */
		const char *v = a + RTA_LENGTH(0);
		size_t vlen = (size_t)rta->rta_len - RTA_LENGTH(0);
		if (rta->rta_type == IFLA_IFNAME && vlen > 0 && vlen <= sizeof name) {
			memcpy(name, v, vlen);
			name[vlen - 1] = '\0';        /* the kernel NUL-terminates; insist */
		}
		a += RTA_ALIGN(rta->rta_len);
	}
	if (!name[0])
		return;                               /* nameless: not ours to judge */
	s->msgs++;

	if (nh->nlmsg_type == RTM_DELLINK) {
		reac_ifscan_gone(s, name, ifi->ifi_index, now_ns);
		return;
	}
	/* Ethernet and not loopback. IFF_LOWER_UP rather than IFLA_CARRIER, for the reason
	 * reac_linkmon.c gives: an admin-down NIC still reports carrier, and it cannot pass a
	 * frame. Wireless is excluded from the SAME gate unless explicitly opted in — see
	 * reac_ifscan.h's header comment. ifi_type == ARPHRD_ETHER is true of a wireless NIC
	 * too, so this is not a second filter layered on top; it is what "ether" now means. */
	int wireless = reac_ifscan_is_wireless(NULL, name);
	int wireless_ok = !wireless ||
	                  reac_ifscan_wireless_allowed(getenv("REAC_IFACES_ALLOW_WIRELESS"), name);
	int ether = ifi->ifi_type == ARPHRD_ETHER && !(ifi->ifi_flags & IFF_LOOPBACK) && wireless_ok;
	reac_ifscan_observe(s, name, ifi->ifi_index, ether,
	                    (ifi->ifi_flags & IFF_LOWER_UP) ? 1 : 0, now_ns);
}

static void feed_ex(struct reac_ifscan *s, const void *buf, size_t len, uint64_t now_ns,
                    int *done)
{
	size_t off = 0;
	while (len - off >= sizeof(struct nlmsghdr)) {
		const struct nlmsghdr *nh = (const struct nlmsghdr *)((const char *)buf + off);
		size_t l = nh->nlmsg_len;
		if (l < sizeof(struct nlmsghdr) || l > len - off)
			break;                            /* malformed: drop the rest */
		if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
			if (done)
				*done = 1;
			break;
		}
		msg_link(s, nh, now_ns);
		off += NLMSG_ALIGN(l);
	}
}

void reac_ifscan_feed(struct reac_ifscan *s, const void *buf, size_t len, uint64_t now_ns)
{
	feed_ex(s, buf, len, now_ns, NULL);
}

int reac_ifscan_resync(struct reac_ifscan *s, uint64_t now_ns)
{
	if (s->fd < 0)
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
	if (send(s->fd, &req, req.nh.nlmsg_len, 0) < 0)
		return -1;

	/* Bounded: a dump that never terminates must not hold the main loop. */
	char buf[IFSCAN_BUF] __attribute__((aligned(8)));
	int done = 0;
	for (int i = 0; i < 64 && !done; i++) {
		struct pollfd p = { .fd = s->fd, .events = POLLIN };
		int pr = poll(&p, 1, 200);
		if (pr <= 0)
			break;
		ssize_t n = recv(s->fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n <= 0)
			break;
		feed_ex(s, buf, (size_t)n, now_ns, &done);
	}
	return done ? 0 : -1;
}

int reac_ifscan_open(struct reac_ifscan *s, uint64_t now_ns)
{
	reac_ifscan_init(s);
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
	s->fd = fd;
	/* Seed from the kernel through the SAME parser every later change uses. */
	if (reac_ifscan_resync(s, now_ns) != 0) {
		close(fd);
		s->fd = -1;
		return -1;
	}
	return 0;
}

int reac_ifscan_fd(const struct reac_ifscan *s)
{
	return s->fd;
}

void reac_ifscan_drain(struct reac_ifscan *s, uint64_t now_ns)
{
	if (s->fd < 0)
		return;
	char buf[IFSCAN_BUF] __attribute__((aligned(8)));
	for (;;) {
		ssize_t n = recv(s->fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n > 0) {
			feed_ex(s, buf, (size_t)n, now_ns, NULL);
			continue;
		}
		if (n < 0 && errno == ENOBUFS) {
			/* The kernel dropped multicast we never saw; the table is a guess.
			 * Ask again instead of trusting it. */
			s->overruns++;
			reac_ifscan_resync(s, now_ns);
			continue;
		}
		break;   /* EAGAIN (drained), or an error worth no action */
	}
}

void reac_ifscan_close(struct reac_ifscan *s)
{
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
}
