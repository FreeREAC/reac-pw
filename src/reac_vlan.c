/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_vlan — rtnetlink create/adopt/mark/remove for `<parent>.<vid>`; see reac_vlan.h.
 */
#include "reac_vlan.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>

/* The up bit in ifi_flags. <linux/if.h> cannot be included beside <net/if.h>, and the
 * value is ABI — the same reasoning as reac_ifscan's IFF_LOWER_UP. */
#ifndef IFF_UP
#define IFF_UP 0x1
#endif

#define VLAN_REQ_BUF 1024
#define VLAN_ANS_BUF 8192

int reac_vlan_name(const char *parent, uint16_t vid, char *out, size_t n)
{
	if (!parent || !out || n == 0)
		return -1;
	int w = snprintf(out, n, "%s.%u", parent, (unsigned)vid);
	if (w < 0 || (size_t)w >= n || w >= IFNAMSIZ) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

/* ---- one request, one answer ------------------------------------------------------- */

struct nlreq {
	struct nlmsghdr  nh;
	struct ifinfomsg ifi;
	char attrs[VLAN_REQ_BUF];
};

static struct rtattr *attr_put(struct nlreq *r, unsigned short type, const void *data,
                               unsigned short len)
{
	size_t off = NLMSG_ALIGN(r->nh.nlmsg_len);
	if (off + RTA_SPACE(len) > sizeof *r)
		return NULL;
	struct rtattr *rta = (struct rtattr *)((char *)r + off);
	rta->rta_type = type;
	rta->rta_len = (unsigned short)RTA_LENGTH(len);
	if (len && data)
		memcpy(RTA_DATA(rta), data, len);
	r->nh.nlmsg_len = (uint32_t)(off + RTA_SPACE(len));
	return rta;
}

static void attr_nest_end(struct nlreq *r, struct rtattr *nest)
{
	nest->rta_len = (unsigned short)((char *)r + r->nh.nlmsg_len - (char *)nest);
}

static int nl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof sa);
	sa.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

/* Send `r` and read the kernel's answer. Returns 0 on an ACK (error 0), -1 with errno set
 * from the kernel's own NLMSG_ERROR otherwise. Bounded: a kernel that never answers must
 * not hold the main loop. */
static int nl_talk(int fd, struct nlreq *r)
{
	static uint32_t seq;
	r->nh.nlmsg_seq = ++seq;
	r->nh.nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	if (send(fd, r, r->nh.nlmsg_len, 0) < 0)
		return -1;

	char buf[VLAN_ANS_BUF] __attribute__((aligned(8)));
	for (int i = 0; i < 16; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int pr = poll(&p, 1, 500);
		if (pr <= 0) {
			errno = pr == 0 ? ETIMEDOUT : errno;
			return -1;
		}
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n <= 0)
			return -1;
		for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (size_t)n);
		     nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_seq != r->nh.nlmsg_seq)
				continue;
			if (nh->nlmsg_type != NLMSG_ERROR)
				continue;
			struct nlmsgerr *e = NLMSG_DATA(nh);
			if (e->error == 0)
				return 0;
			errno = -e->error;
			return -1;
		}
	}
	errno = ETIMEDOUT;
	return -1;
}

/* ---- the four operations ----------------------------------------------------------- */

int reac_vlan_query(const char *name, int *ours)
{
	if (ours)
		*ours = 0;
	if (!name || !name[0])
		return -1;

	int fd = nl_open();
	if (fd < 0)
		return -1;

	struct nlreq req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_GETLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	attr_put(&req, IFLA_IFNAME, name, (unsigned short)(strlen(name) + 1));
	/* IFLA_EXT_MASK is not asked for: the alias comes back in the plain reply. */

	static uint32_t seq;
	req.nh.nlmsg_seq = ++seq + 0x1000;
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}

	char buf[VLAN_ANS_BUF] __attribute__((aligned(8)));
	int present = -1, saved = 0;
	for (int i = 0; i < 16 && present < 0; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (poll(&p, 1, 500) <= 0) {
			saved = ETIMEDOUT;
			break;
		}
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		if (n <= 0) {
			saved = errno;
			break;
		}
		for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (size_t)n);
		     nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_seq != req.nh.nlmsg_seq)
				continue;
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nh);
				/* ENODEV is the answer "there is no such netdev", not a
				 * failure to ask. Anything else is a broken question. */
				if (-e->error == ENODEV) {
					present = 0;
				} else {
					saved = -e->error;
					present = -1;
					goto done;
				}
				break;
			}
			if (nh->nlmsg_type != RTM_NEWLINK)
				continue;
			present = 1;
			struct ifinfomsg *ifi = NLMSG_DATA(nh);
			size_t alen = nh->nlmsg_len - NLMSG_LENGTH(sizeof *ifi);
			for (struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, alen);
			     rta = RTA_NEXT(rta, alen)) {
				if (rta->rta_type != IFLA_IFALIAS)
					continue;
				const char *a = RTA_DATA(rta);
				size_t l = RTA_PAYLOAD(rta);
				if (l && a[l - 1] == '\0' && strcmp(a, REAC_VLAN_ALIAS) == 0 &&
				    ours)
					*ours = 1;
			}
			break;
		}
	}
done:
	close(fd);
	if (present < 0)
		errno = saved ? saved : ETIMEDOUT;
	return present;
}

int reac_vlan_create(const char *parent, uint16_t vid)
{
	char name[IFNAMSIZ];
	if (reac_vlan_name(parent, vid, name, sizeof name) != 0) {
		errno = ENAMETOOLONG;
		return -1;
	}
	unsigned pidx = if_nametoindex(parent);
	if (pidx == 0) {
		errno = ENODEV;
		return -1;
	}
	int fd = nl_open();
	if (fd < 0)
		return -1;

	struct nlreq req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_NEWLINK;
	req.nh.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	req.ifi.ifi_family = AF_UNSPEC;
	uint32_t link = pidx;
	attr_put(&req, IFLA_LINK, &link, sizeof link);
	attr_put(&req, IFLA_IFNAME, name, (unsigned short)(strlen(name) + 1));
	struct rtattr *li = attr_put(&req, IFLA_LINKINFO, NULL, 0);
	attr_put(&req, IFLA_INFO_KIND, "vlan", 4);
	struct rtattr *id = attr_put(&req, IFLA_INFO_DATA, NULL, 0);
	uint16_t v = vid;
	attr_put(&req, IFLA_VLAN_ID, &v, sizeof v);
	attr_nest_end(&req, id);
	attr_nest_end(&req, li);

	int rc = nl_talk(fd, &req);
	int saved = errno;
	close(fd);
	if (rc != 0) {
		errno = saved;
		return -1;
	}

	/* THE MARK AND THE CARRIER, in one further message. IFLA_IFALIAS is honoured by the
	 * kernel's setlink path and not by its create path, so a create that carried the
	 * alias would silently make an UNMARKED netdev — which the next start would read as
	 * the host's and never remove. Marking it here, before anything is served on it,
	 * keeps the window in which a crash can leak an unmarked netdev to microseconds. */
	fd = nl_open();
	if (fd < 0)
		return -1;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_SETLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = (int)if_nametoindex(name);
	req.ifi.ifi_flags = IFF_UP;
	req.ifi.ifi_change = IFF_UP;
	attr_put(&req, IFLA_IFALIAS, REAC_VLAN_ALIAS, (unsigned short)(sizeof REAC_VLAN_ALIAS));
	rc = nl_talk(fd, &req);
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

int reac_vlan_up(const char *name)
{
	unsigned idx = if_nametoindex(name);
	if (idx == 0) {
		errno = ENODEV;
		return -1;
	}
	int fd = nl_open();
	if (fd < 0)
		return -1;
	struct nlreq req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_SETLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = (int)idx;
	req.ifi.ifi_flags = IFF_UP;
	req.ifi.ifi_change = IFF_UP;
	int rc = nl_talk(fd, &req);
	int saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

int reac_vlan_delete(const char *name)
{
	unsigned idx = if_nametoindex(name);
	if (idx == 0) {
		errno = ENODEV;
		return -1;
	}
	int fd = nl_open();
	if (fd < 0)
		return -1;
	struct nlreq req;
	memset(&req, 0, sizeof req);
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
	req.nh.nlmsg_type = RTM_DELLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = (int)idx;
	int rc = nl_talk(fd, &req);
	int saved = errno;
	close(fd);
	errno = saved;
	return rc;
}
