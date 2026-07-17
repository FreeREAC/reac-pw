// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_mac — the stand-in source-MAC helper (task #35, test item D.10). Proves
 * the derived default MAC keeps the Roland OUI, takes its host part from the NIC
 * hwaddr, and can NEVER equal a real box's — the whole point of the change (the
 * old hard-coded 00:40:ab:c4:80:41 IS a real S-1608). The ioctl path is exercised
 * only via the pure reac_mac_compose so the test needs no NIC. */
#include "reac_mac.h"

#include <net/if_arp.h>   /* ARPHRD_ETHER, ARPHRD_LOOPBACK */
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	uint8_t out[6];
	const uint8_t hw[6]    = { 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56 }; /* a NIC hwaddr */
	const uint8_t s1608[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 }; /* a real box */

	/* 1. ETHER hwaddr -> Roland OUI + the NIC's last three bytes. */
	CHK(reac_mac_compose(ARPHRD_ETHER, hw, out) == 0);
	CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);   /* Roland OUI kept */
	CHK(out[3] == 0x12 && out[4] == 0x34 && out[5] == 0x56);   /* host from NIC   */
	/* The derived MAC must not collide with the real S-1608 whose MAC the old
	 * hard-coded stand-in WAS (the entire reason for #35). */
	CHK(memcmp(out, s1608, 6) != 0);

	/* 2. A non-ethernet family (loopback / no hwaddr) falls back, returns -1, but
	 * still yields a usable Roland-OUI MAC with a NON-box, NON-desk host part. */
	CHK(reac_mac_compose(ARPHRD_LOOPBACK, hw, out) == -1);
	CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);
	CHK(!(out[3] == 0x12 && out[4] == 0x34 && out[5] == 0x56));/* NOT NIC-derived */
	CHK(out[3] != 0xc4 && out[3] != 0xc9);                     /* not box/desk class */

	CHK(reac_mac_compose(ARPHRD_ETHER, NULL, out) == -1);      /* NULL hwaddr -> fallback */
	CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);

	/* 3. reac_mac_default_src with no interface -> the same safe fallback, -1. */
	memset(out, 0x55, sizeof out);
	CHK(reac_mac_default_src(NULL, out) == -1);
	CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);
	CHK(out[3] != 0xc4 && out[3] != 0xc9);
	CHK(memcmp(out, s1608, 6) != 0);
	CHK(reac_mac_default_src("", out) == -1);                  /* empty name too */
	CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);

	printf("OK: reac_mac default src = Roland OUI + NIC host part, never a box MAC\n");
	return 0;
}
