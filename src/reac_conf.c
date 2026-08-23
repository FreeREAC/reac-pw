// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *reac_conf_layer_name(enum reac_conf_layer l)
{
	switch (l) {
	case REAC_CONF_ARGV:        return "the command line";
	case REAC_CONF_ENV:         return "the process environment";
	case REAC_CONF_SEGMENT:     return "~/.config/reac-pw/<iface>.env (per-segment)";
	case REAC_CONF_HOST:        return "~/.config/reac-pw/reac-pw.env (per-host)";
	case REAC_CONF_LAST_RESORT: return "~/.config/openmixer/reac.env (last resort)";
	case REAC_CONF_BUILTIN:     return "the built-in default";
	case REAC_CONF_NONE:        break;
	}
	return "nothing";
}

/* Trim ASCII blanks from both ends, in place. Returns the new start. */
static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
	             s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
	return s;
}

int reac_conf_read_file(const char *path, const char *key, char *out, size_t cap)
{
	if (!path || !key || !out || cap == 0)
		return 0;
	FILE *f = fopen(path, "re");
	if (!f)
		return 0;              /* absent is a MISS, never an error: a layer that
		                        * does not exist is a layer with nothing to say */
	char line[1024];
	int hit = 0;
	size_t keylen = strlen(key);
	while (fgets(line, sizeof line, f)) {
		char *p = trim(line);
		if (*p == '#' || *p == '\0')
			continue;
		/* systemd's EnvironmentFile accepts a leading `export`; the existing
		 * files do not use it, but accepting it costs one compare and avoids a
		 * silent miss on a file someone wrote by shell habit. */
		if (!strncmp(p, "export ", 7))
			p = trim(p + 7);
		if (strncmp(p, key, keylen) != 0)
			continue;
		char *eq = trim(p + keylen);
		if (*eq != '=')
			continue;          /* KEY_SOMETHING_ELSE=..., not our key */
		char *v = trim(eq + 1);
		size_t n = strlen(v);
		/* Strip ONE matching pair of surrounding quotes. Not a shell: no
		 * expansion, no escapes, no command substitution. This file is read by a
		 * process holding CAP_NET_RAW. */
		if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
		               (v[0] == '\'' && v[n - 1] == '\''))) {
			v[n - 1] = '\0';
			v++;
		}
		if (*v == '\0')
			continue;          /* blanked out = turned off, not set to "" */
		snprintf(out, cap, "%s", v);
		hit = 1;
		/* No break: LAST assignment in a file wins, which is what a shell and
		 * systemd's EnvironmentFile both do. A file that sets a key twice should
		 * not depend on the reader's scan direction. */
	}
	fclose(f);
	return hit;
}

enum reac_conf_layer reac_conf_lookup(const char *key, const char *iface,
                                      const char *home, char *out, size_t cap)
{
	if (!key || !out || cap == 0)
		return REAC_CONF_NONE;

	/* Layer 2 — the process environment. */
	const char *e = getenv(key);
	if (e && *e) {
		snprintf(out, cap, "%s", e);
		return REAC_CONF_ENV;
	}

	if (!home)
		home = getenv("HOME");
	if (!home || !*home)
		return REAC_CONF_NONE;   /* no home, no files; env was the only chance */

	char path[1024];

	/* Layer 3 — per-segment. Skipped when the caller has no interface, which is
	 * the honest thing to do: a per-segment file cannot answer for "no segment". */
	if (iface && *iface) {
		snprintf(path, sizeof path, "%s/.config/reac-pw/%s.env", home, iface);
		if (reac_conf_read_file(path, key, out, cap))
			return REAC_CONF_SEGMENT;
	}

	/* Layer 4 — per-host, every segment. */
	snprintf(path, sizeof path, "%s/.config/reac-pw/reac-pw.env", home);
	if (reac_conf_read_file(path, key, out, cap))
		return REAC_CONF_HOST;

	/* Layer 5 — the last resort, and the reason this function exists rather than
	 * a getenv. It is what makes a standalone install work with no console
	 * present; openmixer overrides it from above and shows the user the result.
	 * It is the FLOOR of the stack, not a stray duplicate. */
	snprintf(path, sizeof path, "%s/.config/openmixer/reac.env", home);
	if (reac_conf_read_file(path, key, out, cap))
		return REAC_CONF_LAST_RESORT;

	return REAC_CONF_NONE;
}
