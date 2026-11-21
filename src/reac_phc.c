// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The hardware launch's first two conditions (reac_qdisc.h): a PHC that stamps on
 * transmit, and that PHC carrying CLOCK_TAI. In its own file so the qdisc policy's test
 * can replace it at link time: a CI runner has no PHC, and the policy is what is tested. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_qdisc.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

/* A dynamic POSIX clock for a PHC's fd (the kernel's FD_TO_CLOCKID, clock_gettime(2)). */
#define PHC_CLOCKID(fd) ((clockid_t)((((unsigned int)~(fd)) << 3) | 3))

int reac_qdisc_hw_capable(const char *ifname, long long *phc_off_ns, char *why, size_t cap)
{
	if (phc_off_ns)
		*phc_off_ns = 0;
	if (!ifname || !ifname[0]) {
		snprintf(why, cap, "no interface");
		return 0;
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		snprintf(why, cap, "cannot open a socket to ask the driver (errno %d)", errno);
		return 0;
	}
	struct ethtool_ts_info ti;
	struct ifreq ifr;
	memset(&ti, 0, sizeof ti);
	memset(&ifr, 0, sizeof ifr);
	ti.cmd = ETHTOOL_GET_TS_INFO;
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_data = (char *)&ti;
	int rc = ioctl(s, SIOCETHTOOL, &ifr);
	int e = errno;
	close(s);
	if (rc < 0) {
		snprintf(why, cap, "the driver does not say whether it has a hardware clock "
		         "(ETHTOOL_GET_TS_INFO errno %d)", e);
		return 0;
	}
	if (ti.phc_index < 0) {
		snprintf(why, cap, "no PTP hardware clock on this NIC");
		return 0;
	}
	if (!(ti.so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE)) {
		snprintf(why, cap, "PHC /dev/ptp%d but no transmit hardware stamping", ti.phc_index);
		return 0;
	}
	char dev[32];
	snprintf(dev, sizeof dev, "/dev/ptp%d", ti.phc_index);
	int fd = open(dev, O_RDONLY);
	if (fd < 0) {
		snprintf(why, cap, "%s cannot be read (errno %d)", dev, errno);
		return 0;
	}
	/* THE SANDWICH: TAI, PHC, TAI, and the PHC against the midpoint. Best of five, so
	 * one preemption between the reads does not read as misalignment. */
	long long best = 0, best_w = -1;
	for (int i = 0; i < 5; i++) {
		struct timespec a, p, b;
		if (clock_gettime(CLOCK_TAI, &a) || clock_gettime(PHC_CLOCKID(fd), &p) ||
		    clock_gettime(CLOCK_TAI, &b)) {
			e = errno;
			close(fd);
			snprintf(why, cap, "%s cannot be read as a clock (errno %d)", dev, e);
			return 0;
		}
		long long ta = a.tv_sec * 1000000000ll + a.tv_nsec;
		long long tb = b.tv_sec * 1000000000ll + b.tv_nsec;
		long long tp = p.tv_sec * 1000000000ll + p.tv_nsec;
		long long w = tb - ta;
		if (best_w < 0 || w < best_w) {
			best_w = w;
			best = tp - (ta + w / 2);
		}
	}
	close(fd);
	if (phc_off_ns)
		*phc_off_ns = best;
	if (best > REAC_QDISC_PHC_ALIGN_NS || best < -REAC_QDISC_PHC_ALIGN_NS) {
		snprintf(why, cap, "%s is %+.6f s off CLOCK_TAI: nothing aligns it (start "
		         "reac-pw-phc2sys@%s.service, or ptp4l where AES67 owns this NIC)",
		         dev, (double)best / 1e9, ifname);
		return 0;
	}
	snprintf(why, cap, "%s, %+lld ns off CLOCK_TAI", dev, best);
	return 1;
}

