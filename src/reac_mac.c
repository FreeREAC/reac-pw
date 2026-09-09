// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_mac.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>   /* ARPHRD_ETHER */

/* The fallback, used only when the NIC hwaddr can't be read: locally
 * administered (bit 0x02 in the first octet), so it is by construction not any
 * manufacturer's address and cannot collide with real gear on the wire. */
static const uint8_t fallback_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

int reac_mac_compose(int hw_family, const uint8_t hwaddr[6], uint8_t out[6])
{
	if (hw_family == ARPHRD_ETHER && hwaddr) {
		memcpy(out, hwaddr, 6);
		return 0;
	}
	memcpy(out, fallback_mac, 6);
	return -1;
}

int reac_mac_roland_standin(const uint8_t hwaddr[6], uint8_t out[6])
{
	/* Roland's OUI, and this NIC's own host part behind it (see the header). */
	out[0] = 0x00; out[1] = 0x40; out[2] = 0xab;
	out[3] = hwaddr[3]; out[4] = hwaddr[4]; out[5] = hwaddr[5];
	return 0;
}

int reac_mac_default_src(const char *ifname, uint8_t out[6])
{
	/* Fill the fallback first so `out` is valid on every early return. */
	memcpy(out, fallback_mac, 6);

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

uint64_t reac_mac48_pack(const uint8_t mac[6])
{
	uint64_t v = 0;
	if (!mac)
		return 0;
	for (int i = 0; i < 6; i++)
		v = (v << 8) | (uint64_t)mac[i];
	return v;
}

void reac_mac48_unpack(uint64_t packed, uint8_t out[6])
{
	if (!out)
		return;
	for (int i = 5; i >= 0; i--) {
		out[i] = (uint8_t)(packed & 0xffu);
		packed >>= 8;
	}
}
