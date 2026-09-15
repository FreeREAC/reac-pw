// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The instrument for tests/etf-qdisc-owned.sh. Two questions, one binary, because
 * both have to be asked about the SAME namespace and neither may be answered by
 * parsing another program's output.
 *
 *   qdisc <ifname>          what is on the device, read over rtnetlink through the
 *                           library's own public door (reac_etf_qdisc_state) — the
 *                           same reading the pacer makes, not a scrape of
 *                           `tc qdisc show`, which would let a formatting change
 *                           decide a test.
 *
 *   count <ifname> <secs>   how many REAC frames actually ARRIVE at this end of the
 *                           cable in a window. This is the assertion that matters:
 *                           with skip_sock_check an etf qdisc drops every frame
 *                           carrying no launch time, so a daemon on the wrong side
 *                           of that setting transmits NOTHING and the count is the
 *                           only thing that says so. Measured at the FAR end — asking
 *                           the daemon what it sent would be asking the accused.
 *
 * PRESENCE BEFORE ABSENCE: the count prints the total it saw of ANY ethertype beside
 * the REAC total, so a run that read zero REAC frames can be told apart from a probe
 * that was deaf on the wrong interface.
 *
 * Needs CAP_NET_RAW for the counter (AF_PACKET) — it has it inside the test's own
 * user namespace and nowhere else. It never transmits. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reac/transport/reac_etf_qdisc.h>
#include "reac_qdisc.h"

#include <errno.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REAC_ETHERTYPE 0x8819

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int do_qdisc(const char *ifname)
{
	unsigned idx = if_nametoindex(ifname);
	if (!idx) {
		printf("qdisc no-such-device %s\n", ifname);
		return 3;
	}
	char kind[32] = { 0 };
	enum reac_etf_qdisc_state s = reac_etf_qdisc_state((int)idx, kind, sizeof kind);
	printf("qdisc %s root=%s\n",
	       s == REAC_ETF_QDISC_PRESENT ? "etf" :
	       s == REAC_ETF_QDISC_NONE ? "none" : "unreadable",
	       kind[0] ? kind : "(none)");
	return s == REAC_ETF_QDISC_PRESENT ? 0 : s == REAC_ETF_QDISC_NONE ? 1 : 2;
}

static int do_count(const char *ifname, int secs)
{
	unsigned idx = if_nametoindex(ifname);
	if (!idx) {
		fprintf(stderr, "etf_wire_probe: no such device %s\n", ifname);
		return 3;
	}
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (fd < 0) {
		fprintf(stderr, "etf_wire_probe: AF_PACKET: %s (CAP_NET_RAW?)\n",
		        strerror(errno));
		return 4;
	}
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family   = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex  = (int)idx;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		fprintf(stderr, "etf_wire_probe: bind %s: %s\n", ifname, strerror(errno));
		close(fd);
		return 4;
	}

	uint64_t reac = 0, any = 0;
	uint8_t buf[2048];
	const uint64_t stop = mono_ms() + (uint64_t)secs * 1000ull;
	while (mono_ms() < stop) {
		struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
		if (poll(&p, 1, 200) <= 0)
			continue;
		ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n < 14)
			continue;
		any++;
		if (((buf[12] << 8) | buf[13]) == REAC_ETHERTYPE)
			reac++;
	}
	close(fd);
	printf("frames reac=%llu any=%llu secs=%d\n",
	       (unsigned long long)reac, (unsigned long long)any, secs);
	return reac ? 0 : 1;
}

/* stats <ifname> — what the etf qdisc DID: the drop counter the health line reports
 * as launch misses, read through the daemon's OWN door (reac_qdisc_stats_read), never
 * by scraping `tc -s qdisc show`. Same rule as the kind probe above: a formatting
 * change in iproute2 must not be able to decide a test. */
static int do_stats(const char *ifname)
{
	unsigned idx = if_nametoindex(ifname);
	if (!idx) {
		printf("stats no-such-device %s\n", ifname);
		return 3;
	}
	struct reac_qdisc_stats st;
	int rc = reac_qdisc_stats_read((int)idx, &st);
	if (rc != 0) {
		printf("stats unreadable errno=%d\n", -rc);
		return 4;
	}
	printf("stats qdiscs=%u packets=%llu drops=%llu overlimits=%llu\n",
	       st.qdiscs, st.packets, st.drops, st.overlimits);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "qdisc"))
		return do_qdisc(argv[2]);
	if (argc >= 3 && !strcmp(argv[1], "stats"))
		return do_stats(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "count"))
		return do_count(argv[2], atoi(argv[3]));
	fprintf(stderr, "usage: %s qdisc <ifname> | stats <ifname> | count <ifname> <secs>\n",
	        argv[0]);
	return 64;
}
