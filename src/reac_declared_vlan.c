// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* See reac_declared_vlan.h for why a declaration does not wait to be heard. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_declared_vlan.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

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

int reac_declared_vlan_from_key(const char *key, char *parent, size_t cap, uint16_t *vid)
{
	if (!key || strncmp(key, "REAC", 4) != 0)
		return 0;
	const char *us = strrchr(key, '_');
	if (!us || !us[1])
		return 0;
	return reac_declared_vlan_split(us + 1, parent, cap, vid);
}

int reac_declared_vlan_from_filename(const char *fname, char *parent, size_t cap,
                                     uint16_t *vid)
{
	if (!fname)
		return 0;
	const char *base = strrchr(fname, '/');
	base = base ? base + 1 : fname;

	size_t len = strlen(base);
	if (len < 5 || strcmp(base + len - 4, ".env") != 0)
		return 0;
	char seg[IFNAMSIZ + 8];
	if (len - 4 + 1 > sizeof seg)
		return 0;
	memcpy(seg, base, len - 4);
	seg[len - 4] = '\0';
	return reac_declared_vlan_split(seg, parent, cap, vid);
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

int reac_declared_vlan_scan_text(const char *text, struct reac_declared_vlan *tab, int max,
                                 int *n)
{
	if (!text || !tab || !n)
		return 0;
	int added = 0, full = 0;
	const char *p = text;
	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		/* One line, and only its KEY. The value is never read here: naming the
		 * segment IS the declaration, whatever it was named for. */
		char line[256];
		if (len < sizeof line) {
			memcpy(line, p, len);
			line[len] = '\0';
			char *s = line;
			while (*s == ' ' || *s == '\t')
				s++;
			char *eq = strchr(s, '=');
			if (*s && *s != '#' && eq) {
				*eq = '\0';
				/* trim trailing blanks off the key */
				for (char *t = eq - 1; t >= s && (*t == ' ' || *t == '\t'); t--)
					*t = '\0';
				char parent[IFNAMSIZ];
				uint16_t vid;
				if (reac_declared_vlan_from_key(s, parent, sizeof parent, &vid)) {
					int rc = reac_declared_vlan_add(tab, max, n, parent, vid);
					if (rc == 1)
						added++;
					else if (rc < 0)
						full = 1;
				}
			}
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return full ? -1 : added;
}

/* Read a whole small text file into `buf`. Returns 1 on success, 0 when it is not there
 * or does not fit — and a file that does not fit is REPORTED by returning 0 rather than
 * scanned in half, because half a config is a config nobody wrote. */
static int slurp(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, cap - 1, f);
	int over = !feof(f);
	fclose(f);
	if (over)
		return 0;
	buf[n] = '\0';
	return 1;
}

int reac_declared_vlan_scan(struct reac_declared_vlan *tab, int max, const char *home)
{
	if (!tab || max <= 0)
		return -1;
	int n = 0, full = 0;

	/* 1. THE PROCESS ENVIRONMENT — systemd's EnvironmentFile= and an operator's
	 *    one-run override both arrive here, and it is the layer above the files. */
	for (char **e = environ; e && *e; e++) {
		const char *eq = strchr(*e, '=');
		if (!eq || eq == *e)
			continue;
		char key[128];
		size_t klen = (size_t)(eq - *e);
		if (klen + 1 > sizeof key)
			continue;
		memcpy(key, *e, klen);
		key[klen] = '\0';
		char parent[IFNAMSIZ];
		uint16_t vid;
		if (reac_declared_vlan_from_key(key, parent, sizeof parent, &vid))
			if (reac_declared_vlan_add(tab, max, &n, parent, vid) < 0)
				full = 1;
	}

	if (!home)
		home = getenv("HOME");
	if (!home || !*home)
		return full ? -1 : n;

	/* 2. THE TWO CONF FILES, in reac_conf's own order. */
	static const char *files[] = {
		"%s/.config/reac-pw/reac-pw.env",
		"%s/.config/openmixer/reac.env",
	};
	char buf[64 * 1024], path[512];
	for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
		if (snprintf(path, sizeof path, files[i], home) >= (int)sizeof path)
			continue;
		if (slurp(path, buf, sizeof buf))
			if (reac_declared_vlan_scan_text(buf, tab, max, &n) < 0)
				full = 1;
	}

	/* 3. THE PER-SEGMENT FILES. `enp131s0.11.env` declares that segment by existing;
	 *    it needs no key inside it at all, which is what makes it the form an operator
	 *    reaches for when the segment's settings are the same as every other's. */
	if (snprintf(path, sizeof path, "%s/.config/reac-pw", home) < (int)sizeof path) {
		DIR *d = opendir(path);
		if (d) {
			const struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				char parent[IFNAMSIZ];
				uint16_t vid;
				if (reac_declared_vlan_from_filename(de->d_name, parent,
				                                     sizeof parent, &vid))
					if (reac_declared_vlan_add(tab, max, &n, parent, vid) < 0)
						full = 1;
			}
			closedir(d);
		}
	}

	return full ? -1 : n;
}
