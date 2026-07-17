// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_mac.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>   /* ARPHRD_ETHER */

const uint8_t reac_roland_oui[3] = { 0x00, 0x40, 0xab };

/* The fallback host part, used only when the NIC hwaddr can't be read. Boxes sit
 * at 00:40:ab:c4:xx:xx and desks at 00:40:ab:c9:xx:xx (per the RE notes), so a
 * host part of 00:00:01 is outside both device-class ranges and cannot collide. */
static const uint8_t fallback_host[3] = { 0x00, 0x00, 0x01 };

int reac_mac_compose(int hw_family, const uint8_t hwaddr[6], uint8_t out[6])
{
	memcpy(out, reac_roland_oui, 3);
	if (hw_family == ARPHRD_ETHER && hwaddr) {
		memcpy(out + 3, hwaddr + 3, 3);
		return 0;
	}
	memcpy(out + 3, fallback_host, 3);
	return -1;
}

int reac_mac_default_src(const char *ifname, uint8_t out[6])
{
	/* Fill the fallback first so `out` is valid on every early return. */
	memcpy(out, reac_roland_oui, 3);
	memcpy(out + 3, fallback_host, 3);

	if (!ifname || !*ifname)
		return -1;

	/* A plain datagram socket carries SIOCGIFHWADDR — no CAP_NET_RAW, and it
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
