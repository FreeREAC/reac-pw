// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_mac — the default source MAC is THE NIC'S OWN ADDRESS, verbatim
 * (reac_mac.h: a borrowed identity collides with the real device and makes
 * captures ambiguous). The ioctl path is exercised only via the pure
 * reac_mac_compose so the test needs no NIC. */
#include <reac/transport/reac_mac.h>

#include <net/if_arp.h>   /* ARPHRD_ETHER, ARPHRD_LOOPBACK */
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	uint8_t out[6];
	const uint8_t hw[6]    = { 0xaa, 0xbb, 0xcc, 0x12, 0x34, 0x56 }; /* a NIC hwaddr */
	const uint8_t s1608[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 }; /* a real box */
	const uint8_t m200[6]  = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 }; /* a real desk */

	/* 1. ETHER hwaddr -> the NIC's address VERBATIM: our frames carry OUR
	 * identity, no OUI dress-up, no borrowed desk MAC. */
	CHK(reac_mac_compose(ARPHRD_ETHER, hw, out) == 0);
	CHK(memcmp(out, hw, 6) == 0);
	CHK(memcmp(out, s1608, 6) != 0 && memcmp(out, m200, 6) != 0);

	/* 2. A non-ethernet family (loopback / no hwaddr) falls back, returns -1,
	 * and the fallback is LOCALLY ADMINISTERED (02:...) — by construction not
	 * any manufacturer's address, so it can never collide with real gear. */
	CHK(reac_mac_compose(ARPHRD_LOOPBACK, hw, out) == -1);
	CHK(out[0] & 0x02);                                /* locally administered */
	CHK(memcmp(out, hw, 6) != 0);                      /* NOT the NIC address  */
	CHK(memcmp(out, s1608, 6) != 0 && memcmp(out, m200, 6) != 0);

	CHK(reac_mac_compose(ARPHRD_ETHER, NULL, out) == -1);   /* NULL hwaddr -> fallback */
	CHK(out[0] & 0x02);

	/* 3. reac_mac_default_src with no interface -> the same safe fallback, -1. */
	memset(out, 0x55, sizeof out);
	CHK(reac_mac_default_src(NULL, out) == -1);
	CHK(out[0] & 0x02);
	CHK(memcmp(out, s1608, 6) != 0 && memcmp(out, m200, 6) != 0);
	CHK(reac_mac_default_src("", out) == -1);               /* empty name too */
	CHK(out[0] & 0x02);

	/* 0.5.6: the box-master stand-in — Roland's OUI over this NIC's host part, so a
	 * box that only enrols Roland-addressed peers can, and two hosts on one wire stay
	 * distinct. The rest of the daemon keeps the address verbatim. */
	{
		const uint8_t hw[6] = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };
		uint8_t out[6] = { 0 };
		CHK(reac_mac_roland_standin(hw, out) == 0);
		CHK(out[0] == 0x00 && out[1] == 0x40 && out[2] == 0xab);
		CHK(out[3] == 0x9b && out[4] == 0x28 && out[5] == 0x2d);
		/* It is NOT the NIC's address: a caller that wanted that has reac_mac_compose. */
		CHK(memcmp(out, hw, 6) != 0);
	}

	printf("OK: reac_mac default src = the NIC's own address verbatim; "
	       "fallback locally administered, never real gear\n");
	return 0;
}
