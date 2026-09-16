// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* See reac_declared_vlan.h for why a declaration does not wait to be heard. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_declared_vlan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* A netdev name we are prepared to treat as a parent. Deliberately narrow: letters,
 * digits, dot, dash, colon and underscore are what predictable names and their stacked
 * forms use, and anything else in a name that is about to be handed to rtnetlink is a
 * name we did not understand. */
static int name_ok(const char *s)
{
	if (!s || !*s)
		return 0;
	for (const char *p = s; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' &&
		    *p != ':' && *p != '_')
			return 0;
	}
	return 1;
}

int reac_declared_vlan_split(const char *segment, char *parent, size_t cap, uint16_t *vid)
{
	if (!segment || !parent || !cap || !vid)
		return 0;
	const char *dot = strrchr(segment, '.');
	if (!dot || dot == segment || !dot[1])
		return 0;

	/* THE TAIL IS DIGITS OR IT IS NOT A VID. `enp131s0.env` splits at a dot too, and
	 * "env" is exactly the case this refuses. */
	for (const char *p = dot + 1; *p; p++)
		if (!isdigit((unsigned char)*p))
			return 0;
	long v = strtol(dot + 1, NULL, 10);
	if (v < 1 || v > 4094)
		return 0;

	size_t plen = (size_t)(dot - segment);
	if (plen + 1 > cap || plen + 1 > IFNAMSIZ)
		return 0;
	/* AND THE WHOLE NETDEV NAME HAS TO FIT, not just the parent: `<parent>.<vid>` is
	 * what will be created, and a truncated interface name is a DIFFERENT interface. */
	if (strlen(segment) + 1 > IFNAMSIZ)
		return 0;
	memcpy(parent, segment, plen);
	parent[plen] = '\0';
	if (!name_ok(parent))
		return 0;
	*vid = (uint16_t)v;
	return 1;
}


int reac_declared_vlan_add(struct reac_declared_vlan *tab, int max, int *n,
                           const char *parent, uint16_t vid)
{
	if (!tab || !n || !parent)
		return -1;
	for (int i = 0; i < *n; i++)
		if (tab[i].vid == vid && strcmp(tab[i].parent, parent) == 0)
			return 0;
	if (*n >= max)
		return -1;
	memset(&tab[*n], 0, sizeof tab[*n]);
	strncpy(tab[*n].parent, parent, sizeof tab[*n].parent - 1);
	tab[*n].vid = vid;
	(*n)++;
	return 1;
}


