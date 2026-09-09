// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_mac.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

int reac_mac_default_src(const char *ifname, uint8_t out[6])
{
	/* Fill the fallback first so `out` is valid on every early return. */
	reac_mac_fallback(out);

	if (!ifname || !*ifname)
		return -1;

	/* A plain datagram socket carries SIOCGIFHWADDR - no CAP_NET_RAW, and it
	 * only READS the interface address (never brings the NIC up or changes it). */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	int rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
	close(fd);
	if (rc < 0)
		return -1;

	return reac_mac_compose(ifr.ifr_hwaddr.sa_family,
	                        (const uint8_t *)ifr.ifr_hwaddr.sa_data, out);
}
