// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The rtnetlink side of reac_link_alias.h: one RTM_GETLINK and one RTM_NEWLINK, nothing
 * else. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_link_alias.h"

#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* linux/if.h has it, and cannot be included beside net/if.h. */
#ifndef IFALIASZ
#define IFALIASZ 256
#endif

static void copy_attr(const struct rtattr *a, char *out, size_t n)
{
	if (!out || !n)
		return;
	size_t len = RTA_PAYLOAD(a);
	const char *s = RTA_DATA(a);
	size_t k = strnlen(s, len);
	if (k >= n)
		k = n - 1;
	memcpy(out, s, k);
	out[k] = '\0';
}

/* Send one request and read the answer until its ACK or its RTM_NEWLINK. */
static int nl_talk(void *req, size_t len, char *kind, size_t kind_n,
                   char *alias, size_t alias_n)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (send(fd, req, len, 0) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	char buf[16384];
	int rc = -EIO;
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n < 0) {
			rc = -errno;
			break;
		}
		struct nlmsghdr *nh = (struct nlmsghdr *)buf;
		for (; NLMSG_OK(nh, (unsigned)n); nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *err = NLMSG_DATA(nh);
				rc = err->error; /* 0 is the ACK */
				goto out;
			}
			if (nh->nlmsg_type != RTM_NEWLINK)
				continue;
			const struct ifinfomsg *ifi = NLMSG_DATA(nh);
			int alen = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof *ifi));
			for (const struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, alen);
			     a = RTA_NEXT(a, alen)) {
				if (a->rta_type == IFLA_IFALIAS) {
					copy_attr(a, alias, alias_n);
				} else if (a->rta_type == IFLA_LINKINFO) {
					int ilen = (int)RTA_PAYLOAD(a);
					for (const struct rtattr *i = RTA_DATA(a); RTA_OK(i, ilen);
					     i = RTA_NEXT(i, ilen))
						if (i->rta_type == IFLA_INFO_KIND)
							copy_attr(i, kind, kind_n);
				}
			}
			rc = 0;
			goto out;
		}
	}
out:
	close(fd);
	return rc;
}

int reac_link_alias_read(const char *name, char *kind, size_t kind_n,
                         char *alias, size_t alias_n)
{
	if (kind && kind_n)
		kind[0] = '\0';
	if (alias && alias_n)
		alias[0] = '\0';
	if (!name || !name[0])
		return -EINVAL;
	unsigned idx = if_nametoindex(name);
	if (!idx)
		return -ENODEV;
	struct {
		struct nlmsghdr  nh;
		struct ifinfomsg ifi;
	} req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type  = RTM_GETLINK;
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	req.nh.nlmsg_seq   = 1;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index  = (int)idx;
	return nl_talk(&req, req.nh.nlmsg_len, kind, kind_n, alias, alias_n);
}

int reac_link_alias_write(const char *name, const char *alias)
{
	if (!name || !name[0] || !alias)
		return -EINVAL;
	size_t alen = strlen(alias);
	if (alen >= IFALIASZ)
		return -ENAMETOOLONG;
	unsigned idx = if_nametoindex(name);
	if (!idx)
		return -ENODEV;
	struct {
		struct nlmsghdr  nh;
		struct ifinfomsg ifi;
		char             attrs[RTA_SPACE(IFALIASZ)];
	} req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_type  = RTM_NEWLINK;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	req.nh.nlmsg_seq   = 1;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index  = (int)idx;
	/* An EMPTY alias attribute is how the kernel is told to remove the alias. */
	struct rtattr *a = (struct rtattr *)req.attrs;
	a->rta_type = IFLA_IFALIAS;
	a->rta_len  = (unsigned short)RTA_LENGTH(alen);
	memcpy(RTA_DATA(a), alias, alen);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi) + RTA_ALIGN(a->rta_len);
	return nl_talk(&req, req.nh.nlmsg_len, NULL, 0, NULL, 0);
}
