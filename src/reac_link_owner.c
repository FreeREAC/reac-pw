// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_link_owner: see reac_link_owner.h for the defect and the three facts. This file is
// the pure verdict and the three rtnetlink dumps that feed it.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_link_owner.h"

#include <errno.h>
#include <stdint.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum reac_link_owner reac_link_owner_verdict(const struct reac_link_owner_facts *f)
{
	if (!f || f->err)
		return REAC_LINK_OWNER_UNREAD;
	if (f->addrs > 0)
		return REAC_LINK_OWNER_ADDR;
	if (f->stacked > 0)
		return REAC_LINK_OWNER_STACKED;
	if (f->default_routes > 0)
		return REAC_LINK_OWNER_ROUTE;
	return REAC_LINK_OWNER_BARE;
}

int reac_link_owner_is_hosts(enum reac_link_owner v)
{
	return v != REAC_LINK_OWNER_BARE;
}

const char *reac_link_owner_text(enum reac_link_owner v)
{
	switch (v) {
	case REAC_LINK_OWNER_BARE:    return "a wire of ours and nothing else";
	case REAC_LINK_OWNER_ADDR:    return "the host's: it carries an address beyond link scope";
	case REAC_LINK_OWNER_STACKED: return "the host's: a sub-interface is stacked on it, or it "
	                                     "is enslaved";
	case REAC_LINK_OWNER_ROUTE:   return "the host's: a default route goes out of it";
	case REAC_LINK_OWNER_UNREAD:  return "unreadable from rtnetlink, so treated as the host's";
	}
	return "unreadable from rtnetlink, so treated as the host's";
}

/* ---- the dumps ---------------------------------------------------------------------- */

typedef void (*nl_msg_fn)(const struct nlmsghdr *nh, void *ctx);

/* One dump request, the whole multi-part answer handed message by message to `fn`.
 * Returns 0, or -errno. The request header is whatever the family's dump expects
 * (ifaddrmsg, ifinfomsg, rtmsg), all with AF_UNSPEC so one dump covers IPv4 and IPv6. */
static int nl_dump(uint16_t type, void *hdr, size_t hdr_len, nl_msg_fn fn, void *ctx)
{
	struct {
		struct nlmsghdr nh;
		unsigned char   body[64];
	} req;
	if (hdr_len > sizeof req.body)
		return -EINVAL;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len   = (uint32_t)NLMSG_LENGTH(hdr_len);
	req.nh.nlmsg_type  = type;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nh.nlmsg_seq   = 1;
	memcpy(req.body, hdr, hdr_len);

	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	static __thread char buf[65536];
	int rc = 0;
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0) {
			rc = -EIO;
			break;
		}
		int done = 0;
		const struct nlmsghdr *nh = (const struct nlmsghdr *)buf;
		for (; NLMSG_OK(nh, (unsigned)n); nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type == NLMSG_DONE) {
				done = 1;
				break;
			}
			if (nh->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *err = NLMSG_DATA(nh);
				rc = err->error ? err->error : -EIO;
				done = 1;
				break;
			}
			fn(nh, ctx);
		}
		if (done)
			break;
	}
	close(fd);
	return rc;
}

struct owner_ctx {
	int idx;
	struct reac_link_owner_facts *f;
};

/* An address on this interface whose scope is wider than link. RT_SCOPE_UNIVERSE is 0 and
 * RT_SCOPE_SITE 200; RT_SCOPE_LINK is 253, HOST 254, NOWHERE 255 (linux/rtnetlink.h). */
static void on_addr(const struct nlmsghdr *nh, void *ctx)
{
	struct owner_ctx *c = ctx;
	if (nh->nlmsg_type != RTM_NEWADDR)
		return;
	const struct ifaddrmsg *ifa = NLMSG_DATA(nh);
	if ((int)ifa->ifa_index != c->idx)
		return;
	if (ifa->ifa_scope < RT_SCOPE_LINK)
		c->f->addrs++;
}

/* A netdev whose lower link is this interface (IFLA_LINK), of a kind that stacks on a
 * parent; a veth names its PEER in IFLA_LINK and stacks on nothing. A link whose peer is
 * in another namespace (IFLA_LINK_NETNSID) is not stacked on ours either. And this
 * interface itself carrying IFLA_MASTER is enslaved to a bridge or a bond. */
static void on_link(const struct nlmsghdr *nh, void *ctx)
{
	struct owner_ctx *c = ctx;
	if (nh->nlmsg_type != RTM_NEWLINK)
		return;
	const struct ifinfomsg *ifi = NLMSG_DATA(nh);
	int alen = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof *ifi));
	int link = 0, other_ns = 0, master = 0;
	char kind[32] = "";
	for (const struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
		if (a->rta_type == IFLA_LINK && RTA_PAYLOAD(a) >= sizeof(int)) {
			memcpy(&link, RTA_DATA(a), sizeof link);
		} else if (a->rta_type == IFLA_LINK_NETNSID) {
			other_ns = 1;
		} else if (a->rta_type == IFLA_MASTER) {
			master = 1;
		} else if (a->rta_type == IFLA_LINKINFO) {
			int ilen = (int)RTA_PAYLOAD(a);
			for (const struct rtattr *i = RTA_DATA(a); RTA_OK(i, ilen); i = RTA_NEXT(i, ilen))
				if (i->rta_type == IFLA_INFO_KIND) {
					size_t k = RTA_PAYLOAD(i);
					if (k >= sizeof kind)
						k = sizeof kind - 1;
					memcpy(kind, RTA_DATA(i), k);
					kind[strnlen(kind, k)] = '\0';
				}
		}
	}
	if (ifi->ifi_index == c->idx) {
		if (master)
			c->f->stacked++;
		return;
	}
	if (link != c->idx || other_ns)
		return;
	if (strcmp(kind, "vlan") == 0 || strcmp(kind, "macvlan") == 0 ||
	    strcmp(kind, "macvtap") == 0 || strcmp(kind, "ipvlan") == 0)
		c->f->stacked++;
}

/* A unicast route to everything (dst /0) whose output interface is this one, any table. */
static void on_route(const struct nlmsghdr *nh, void *ctx)
{
	struct owner_ctx *c = ctx;
	if (nh->nlmsg_type != RTM_NEWROUTE)
		return;
	const struct rtmsg *rtm = NLMSG_DATA(nh);
	if (rtm->rtm_dst_len != 0 || rtm->rtm_type != RTN_UNICAST)
		return;
	int alen = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof *rtm));
	for (const struct rtattr *a = RTM_RTA(rtm); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
		if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= sizeof(int)) {
			int oif;
			memcpy(&oif, RTA_DATA(a), sizeof oif);
			if (oif == c->idx)
				c->f->default_routes++;
		}
	}
}

enum reac_link_owner reac_link_owner_read(const char *ifname, struct reac_link_owner_facts *f)
{
	struct reac_link_owner_facts z = { 0 };
	if (!f)
		f = &z;
	memset(f, 0, sizeof *f);
	if (!ifname || !ifname[0]) {
		f->err = -EINVAL;
		return REAC_LINK_OWNER_UNREAD;
	}
	unsigned idx = if_nametoindex(ifname);
	if (!idx) {
		f->err = -ENODEV;
		return REAC_LINK_OWNER_UNREAD;
	}
	struct owner_ctx c = { .idx = (int)idx, .f = f };
	struct ifaddrmsg ifa = { .ifa_family = AF_UNSPEC };
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC };
	struct rtmsg     rtm = { .rtm_family = AF_UNSPEC };
	int rc;
	if ((rc = nl_dump(RTM_GETADDR, &ifa, sizeof ifa, on_addr, &c)) != 0 ||
	    (rc = nl_dump(RTM_GETLINK, &ifi, sizeof ifi, on_link, &c)) != 0 ||
	    (rc = nl_dump(RTM_GETROUTE, &rtm, sizeof rtm, on_route, &c)) != 0) {
		f->err = rc;
		return REAC_LINK_OWNER_UNREAD;
	}
	return reac_link_owner_verdict(f);
}
