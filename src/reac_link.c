/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
#include "reac_link.h"

#include <net/if.h>   /* IFNAMSIZ */
#include <stdio.h>

int reac_link_carrier(const char *ifname)
{
	if (!ifname || !*ifname)
		return -1;
	char path[IFNAMSIZ + 32];
	int n = snprintf(path, sizeof path, "/sys/class/net/%s/carrier", ifname);
	if (n < 0 || (size_t)n >= sizeof path)
		return -1;   /* too long to be an interface name: UNKNOWN, not "down" */
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;   /* gone, or not readable: UNKNOWN, never "down" */
	int c = fgetc(f);
	fclose(f);
	if (c == '1')
		return 1;
	if (c == '0')
		return 0;
	return -1;
}
