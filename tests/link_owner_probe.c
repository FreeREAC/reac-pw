// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Reads one interface through reac_link_owner and prints one line:
 *   <bare|addr|stacked|route|unread> addrs=N stacked=N routes=N err=E
 * The namespace test link-owner-reads-the-netdev.sh drives it. */
#include "reac_link_owner.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s IFNAME\n", argv[0]);
		return 2;
	}
	struct reac_link_owner_facts f;
	const enum reac_link_owner v = reac_link_owner_read(argv[1], &f);
	static const char *word[] = { "bare", "addr", "stacked", "route", "unread" };
	printf("%s addrs=%d stacked=%d routes=%d err=%d\n",
	       (unsigned)v < 5 ? word[v] : "unread", f.addrs, f.stacked, f.default_routes, f.err);
	return 0;
}
