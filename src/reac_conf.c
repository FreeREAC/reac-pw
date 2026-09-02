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
	case REAC_CONF_SEGMENT:     return "a per-segment key (<KEY>_<segment>) in the environment or a conf file";
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

/* The bare key through layers 2, 4 and 5, in that order. `home` is non-NULL here. */
static enum reac_conf_layer lookup_bare(const char *key, const char *home,
                                        char *out, size_t cap)
{
	/* Layer 2 — the process environment. */
	const char *e = getenv(key);
	if (e && *e) {
		snprintf(out, cap, "%s", e);
		return REAC_CONF_ENV;
	}

	char path[1024];

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

enum reac_conf_layer reac_conf_lookup(const char *key, const char *segment,
                                      const char *home, char *out, size_t cap)
{
	if (!key || !out || cap == 0)
		return REAC_CONF_NONE;

	if (!home)
		home = getenv("HOME");
	if (!home || !*home) {
		/* No home, no files; the environment is the only chance, and the
		 * per-segment key still outranks the bare one there. */
		char seg_key[256];
		if (segment && *segment) {
			snprintf(seg_key, sizeof seg_key, "%s_%s", key, segment);
			const char *e = getenv(seg_key);
			if (e && *e) {
				snprintf(out, cap, "%s", e);
				return REAC_CONF_SEGMENT;
			}
		}
		const char *e = getenv(key);
		if (!e || !*e)
			return REAC_CONF_NONE;
		snprintf(out, cap, "%s", e);
		return REAC_CONF_ENV;
	}

	/* Layer 3 — per-segment: the key SUFFIXED with the segment's name, in any
	 * of the layers below, before the bare key in any of them. Skipped when the
	 * caller has no segment, which is the honest thing to do: a per-segment key
	 * cannot answer for "no segment". It sits ABOVE the bare key in every layer
	 * because systemd's EnvironmentFile= exports reac-pw.env into the process
	 * environment: a bare REAC_ROLE=master there must not outrank the
	 * REAC_ROLE_<segment> the console wrote for one segment in the same file. */
	if (segment && *segment) {
		char seg_key[256];
		snprintf(seg_key, sizeof seg_key, "%s_%s", key, segment);
		if (lookup_bare(seg_key, home, out, cap) != REAC_CONF_NONE)
			return REAC_CONF_SEGMENT;
	}

	return lookup_bare(key, home, out, cap);
}
