// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_own_macs.h"

#include <dirent.h>
#include <net/ethernet.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "reac_facts_pw.h"

/* The source address is where Ethernet puts it (IEEE 802.3), whatever the REAC payload's
 * layout: no protocol fact moves it. */
#define ETH_SRC_OFF offsetof(struct ether_header, ether_shost)

static int read_address(const char *netdir, const char *name, uint8_t mac[6])
{
	char path[512], line[64];
	if (snprintf(path, sizeof path, "%s/%s/address", netdir, name) >= (int)sizeof path)
		return -1;
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char *got = fgets(line, sizeof line, f);
	fclose(f);
	unsigned b[6];
	if (!got || sscanf(line, "%2x:%2x:%2x:%2x:%2x:%2x",
	                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		mac[i] = (uint8_t)b[i];
	return 0;
}

int reac_own_macs_has(const struct reac_own_macs *s, const uint8_t mac[6])
{
	for (unsigned i = 0; i < s->n; i++)
		if (memcmp(s->mac[i], mac, 6) == 0)
			return 1;
	return 0;
}

int reac_own_macs_read(struct reac_own_macs *s, const char *netdir)
{
	if (!netdir)
		netdir = REAC_OWN_MACS_SYS;
	DIR *d = opendir(netdir);
	if (!d)
		return -1;
	struct reac_own_macs fresh = { 0 };
	static const uint8_t zero[6];
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		uint8_t mac[6];
		if (e->d_name[0] == '.' || read_address(netdir, e->d_name, mac) != 0)
			continue;
		if (memcmp(mac, zero, 6) == 0 || reac_own_macs_has(&fresh, mac))
			continue;
		if (fresh.n < REAC_OWN_MACS_MAX)
			memcpy(fresh.mac[fresh.n++], mac, 6);
	}
	closedir(d);
	*s = fresh;
	return (int)fresh.n;
}

enum reac_own_echo reac_own_echo_screen(const struct reac_own_macs *s, int *said,
                                        const uint8_t *frame, size_t n)
{
	if (n < ETH_SRC_OFF + 6 || !reac_own_macs_has(s, frame + ETH_SRC_OFF))
		return REAC_OWN_ECHO_NONE;
	if (*said)
		return REAC_OWN_ECHO_AGAIN;
	*said = 1;
	return REAC_OWN_ECHO_FIRST;
}
