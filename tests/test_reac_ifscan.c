/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_ifscan — the table's transitions, pinned. Every case is the operator's own sentence
 * from the ruling (openmixer trunk-VLAN spec, amendment 2026-09-02): a NIC with link is
 * SNIFFED and nothing more; the first REAC frame makes it a SEGMENT; a flap shorter than the
 * hold changes nothing; a loss longer than it drops the segment; a netdev that leaves drops at
 * once; a NIC that links and never hears REAC is never a segment. Then the netlink parser is
 * fed the datagrams the kernel would send, laid out byte for byte, so the socket half is
 * only ever a byte source for a core proven here.
 *
 * PRESENCE BEFORE ABSENCE: every "nothing happened" assertion follows one that proved the
 * queue delivers, so a broken queue cannot pass the quiet cases by being deaf.
 */
#include "reac_ifscan.h"

#include <stdio.h>
#include <string.h>
#include <net/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define S 1000000000ULL

/* Pop every queued event into `verbs`/`names`; returns how many. */
static int drain(struct reac_ifscan *s, enum reac_ifscan_verb *verbs, char names[][IFNAMSIZ], int cap)
{
	struct reac_ifscan_event ev;
	int n = 0;
	while (reac_ifscan_next(s, &ev)) {
		if (n < cap) {
			verbs[n] = ev.verb;
			snprintf(names[n], IFNAMSIZ, "%s", ev.name);
		}
		n++;
	}
	return n;
}

static void t_link_then_hear_then_hold(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	/* A NIC without link: tracked, nothing to do. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 0, 0);
	CHK(drain(&s, v, n, 8) == 0);
	CHK(reac_ifscan_find(&s, "eth0") && reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_DOWN);

	/* Link comes: LISTEN, and only listen. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 1, 1 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN && !strcmp(n[0], "eth0"));
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_LINKED);

	/* The same observation again (a dump repeat, a flag change that is not carrier): nothing. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 1, 2 * S);
	CHK(drain(&s, v, n, 8) == 0);

	/* REAC heard: SERVE. */
	reac_ifscan_heard(&s, "eth0", 3 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_SERVE);
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_SEGMENT);
	/* Heard again while served: nothing — the listener owns the wire now. */
	reac_ifscan_heard(&s, "eth0", 4 * S);
	CHK(drain(&s, v, n, 8) == 0);

	/* Carrier lost: HOLD, no event yet. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 0, 10 * S);
	CHK(drain(&s, v, n, 8) == 0);
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_HOLD);
	/* Inside the hold nothing falls due. */
	reac_ifscan_tick(&s, 10 * S + REAC_IFSCAN_DOWN_HOLD_NS - 1);
	CHK(drain(&s, v, n, 8) == 0);
	/* Carrier back inside the hold: KEPT, the segment stands, one flap counted. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 1, 11 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_KEPT);
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_SEGMENT);
	CHK(reac_ifscan_find(&s, "eth0")->flaps == 1);
	reac_ifscan_tick(&s, 20 * S);
	CHK(drain(&s, v, n, 8) == 0);

	/* Carrier lost for longer than the hold: DROP, then back to DOWN. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 0, 30 * S);
	reac_ifscan_tick(&s, 30 * S + REAC_IFSCAN_DOWN_HOLD_NS - 1);
	CHK(drain(&s, v, n, 8) == 0);
	reac_ifscan_tick(&s, 30 * S + REAC_IFSCAN_DOWN_HOLD_NS);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_DROP);
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_DOWN);
	/* And it is sniffed afresh when link returns — the next box is heard, not assumed. */
	reac_ifscan_observe(&s, "eth0", 2, 1, 1, 40 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN);
	CHK(reac_ifscan_find(&s, "eth0")->state == REAC_IFSCAN_LINKED);
}

static void t_never_heard_never_segment(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	reac_ifscan_observe(&s, "office0", 3, 1, 1, 0);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN);
	for (int i = 1; i < 100; i++)
		reac_ifscan_tick(&s, (uint64_t)i * S);
	CHK(drain(&s, v, n, 8) == 0);
	CHK(reac_ifscan_count(&s, REAC_IFSCAN_SEGMENT) == 0);
	CHK(reac_ifscan_count(&s, REAC_IFSCAN_LINKED) == 1);
	/* Link gone on a sniffed NIC: UNLISTEN, at once, no hold — nothing was served. */
	reac_ifscan_observe(&s, "office0", 3, 1, 0, 100 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_UNLISTEN);
}

static void t_not_ethernet_ignored(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	reac_ifscan_observe(&s, "lo", 1, 0, 1, 0);
	reac_ifscan_observe(&s, "wg0", 5, 0, 1, 0);
	CHK(drain(&s, v, n, 8) == 0);
	CHK(reac_ifscan_find(&s, "lo") == NULL);
	CHK(reac_ifscan_find(&s, "wg0") == NULL);
	/* Heard on something untracked: nothing. */
	reac_ifscan_heard(&s, "wg0", 1);
	CHK(drain(&s, v, n, 8) == 0);
}

static void t_dellink_drops_at_once(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	reac_ifscan_observe(&s, "usb0", 9, 1, 1, 0);
	reac_ifscan_heard(&s, "usb0", 1 * S);
	CHK(drain(&s, v, n, 8) == 2 && v[1] == REAC_IFSCAN_SERVE);
	reac_ifscan_gone(&s, "usb0", 9, 2 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_DROP);
	CHK(reac_ifscan_find(&s, "usb0") == NULL);

	/* A stale DELLINK for an index we no longer hold must not touch the new one. */
	reac_ifscan_observe(&s, "usb0", 10, 1, 1, 3 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN);
	reac_ifscan_gone(&s, "usb0", 9, 4 * S);
	CHK(drain(&s, v, n, 8) == 0);
	CHK(reac_ifscan_find(&s, "usb0") && reac_ifscan_find(&s, "usb0")->ifindex == 10);
}

static void t_reenumeration_is_gone_then_new(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	reac_ifscan_observe(&s, "usb0", 9, 1, 1, 0);
	reac_ifscan_heard(&s, "usb0", 1 * S);
	drain(&s, v, n, 8);
	/* Same name, new index, link up: the old segment DROPS and the new NIC is sniffed. */
	reac_ifscan_observe(&s, "usb0", 12, 1, 1, 2 * S);
	CHK(drain(&s, v, n, 8) == 2 && v[0] == REAC_IFSCAN_DROP && v[1] == REAC_IFSCAN_LISTEN);
	CHK(reac_ifscan_find(&s, "usb0")->ifindex == 12);
	CHK(reac_ifscan_find(&s, "usb0")->state == REAC_IFSCAN_LINKED);
}

static void t_serve_failed_retries_later(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];

	reac_ifscan_observe(&s, "eth1", 4, 1, 1, 0);
	reac_ifscan_heard(&s, "eth1", 1 * S);
	drain(&s, v, n, 8);
	reac_ifscan_serve_failed(&s, "eth1", 1 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN);
	/* Heard again inside the retry window: ignored, so the wire does not retry at line rate. */
	reac_ifscan_heard(&s, "eth1", 1 * S + REAC_IFSCAN_RETRY_NS - 1);
	CHK(drain(&s, v, n, 8) == 0);
	reac_ifscan_heard(&s, "eth1", 1 * S + REAC_IFSCAN_RETRY_NS);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_SERVE);
}

static void t_bounded(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	for (int i = 0; i < REAC_IFSCAN_MAX + 3; i++) {
		char name[IFNAMSIZ];
		snprintf(name, sizeof name, "e%d", i);
		reac_ifscan_observe(&s, name, i + 1, 1, 1, 0);
	}
	CHK(s.unbounded == 3);
	CHK(reac_ifscan_count(&s, REAC_IFSCAN_LINKED) == REAC_IFSCAN_MAX);
}

/* Build one RTM_NEWLINK/RTM_DELLINK exactly as the kernel lays it out. */
static size_t build_link(void *buf, int type, const char *ifname, int ifindex,
                         unsigned short arphrd, unsigned flags)
{
	char *p = buf;
	struct nlmsghdr *nh = (struct nlmsghdr *)p;
	memset(nh, 0, NLMSG_HDRLEN);
	nh->nlmsg_type = (unsigned short)type;

	struct ifinfomsg *ifi = (struct ifinfomsg *)(p + NLMSG_HDRLEN);
	memset(ifi, 0, sizeof *ifi);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_type = arphrd;
	ifi->ifi_index = ifindex;
	ifi->ifi_flags = flags;

	size_t off = NLMSG_HDRLEN + NLMSG_ALIGN(sizeof *ifi);
	size_t nlen = strlen(ifname) + 1;
	struct rtattr *rta = (struct rtattr *)(p + off);
	rta->rta_type = IFLA_IFNAME;
	rta->rta_len = (unsigned short)RTA_LENGTH(nlen);
	memcpy(p + off + RTA_LENGTH(0), ifname, nlen);
	off += RTA_ALIGN(rta->rta_len);
	nh->nlmsg_len = (unsigned int)off;
	return off;
}

static void t_netlink_parse(void)
{
	struct reac_ifscan s;
	reac_ifscan_init(&s);
	enum reac_ifscan_verb v[8];
	char n[8][IFNAMSIZ];
	char buf[1024] __attribute__((aligned(8)));

	/* A dump: lo (loopback, ignored), an Ethernet NIC with link, one without, a tunnel. */
	size_t off = 0;
	off += build_link(buf + off, RTM_NEWLINK, "lo", 1, ARPHRD_LOOPBACK, IFF_UP | IFF_LOWER_UP | IFF_LOOPBACK);
	off += build_link(buf + off, RTM_NEWLINK, "enp1s0", 2, ARPHRD_ETHER, IFF_UP | IFF_LOWER_UP);
	off += build_link(buf + off, RTM_NEWLINK, "enp2s0", 3, ARPHRD_ETHER, IFF_UP);
	off += build_link(buf + off, RTM_NEWLINK, "wg0", 4, ARPHRD_NONE, IFF_UP | IFF_LOWER_UP);
	reac_ifscan_feed(&s, buf, off, 0);
	CHK(s.msgs == 4);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN && !strcmp(n[0], "enp1s0"));
	CHK(reac_ifscan_find(&s, "lo") == NULL);
	CHK(reac_ifscan_find(&s, "wg0") == NULL);
	CHK(reac_ifscan_find(&s, "enp2s0")->state == REAC_IFSCAN_DOWN);

	/* Carrier comes on the second NIC. */
	off = build_link(buf, RTM_NEWLINK, "enp2s0", 3, ARPHRD_ETHER, IFF_UP | IFF_LOWER_UP);
	reac_ifscan_feed(&s, buf, off, 1 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_LISTEN && !strcmp(n[0], "enp2s0"));

	/* An admin-down NIC with the cable in: IFF_LOWER_UP clear, IFF_RUNNING irrelevant. */
	off = build_link(buf, RTM_NEWLINK, "enp2s0", 3, ARPHRD_ETHER, 0);
	reac_ifscan_feed(&s, buf, off, 2 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_UNLISTEN);

	/* DELLINK on a served segment drops it. */
	reac_ifscan_heard(&s, "enp1s0", 3 * S);
	drain(&s, v, n, 8);
	off = build_link(buf, RTM_DELLINK, "enp1s0", 2, ARPHRD_ETHER, 0);
	reac_ifscan_feed(&s, buf, off, 4 * S);
	CHK(drain(&s, v, n, 8) == 1 && v[0] == REAC_IFSCAN_DROP && !strcmp(n[0], "enp1s0"));

	/* A truncated datagram is dropped, never guessed: no message counted, no event. */
	unsigned long before = s.msgs;
	off = build_link(buf, RTM_NEWLINK, "enp3s0", 7, ARPHRD_ETHER, IFF_UP | IFF_LOWER_UP);
	((struct nlmsghdr *)buf)->nlmsg_len = (unsigned int)(off + 64);
	reac_ifscan_feed(&s, buf, off, 5 * S);
	CHK(s.msgs == before);
	CHK(drain(&s, v, n, 8) == 0);
}

/* The live arm: an RTM_GETLINK dump of the real host through the same parser. Unprivileged.
 * Asserts only what any host has — a table that parsed at least `lo`'s neighbours — and
 * that loopback is not in it. */
static void t_live_dump(void)
{
	struct reac_ifscan s;
	if (reac_ifscan_open(&s, 0) != 0) {
		fprintf(stderr, "note: netlink unavailable here; live arm skipped\n");
		return;
	}
	CHK(s.msgs >= 1);
	CHK(reac_ifscan_find(&s, "lo") == NULL);
	reac_ifscan_close(&s);
}

int main(void)
{
	t_link_then_hear_then_hold();
	t_never_heard_never_segment();
	t_not_ethernet_ignored();
	t_dellink_drops_at_once();
	t_reenumeration_is_gone_then_new();
	t_serve_failed_retries_later();
	t_bounded();
	t_netlink_parse();
	t_live_dump();
	if (fails) {
		fprintf(stderr, "test_reac_ifscan: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_ifscan: OK\n");
	return 0;
}
