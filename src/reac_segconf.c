// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_segconf.h"
#include "reac_declared_vlan.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */

void reac_segconf_init(struct reac_segconf *c)
{
	memset(c, 0, sizeof *c);
}

/* A refusal is kept VERBATIM while there is room and COUNTED for ever. The count is what
 * lets the caller say "and 12 more", which is the difference between a bound the operator
 * can act on and one that quietly swallows the rest of their file. */
static void refuse(struct reac_segconf *c, int line, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void refuse(struct reac_segconf *c, int line, const char *fmt, ...)
{
	c->refused++;
	if (c->n_refusals >= REAC_SEGCONF_REFUSALS)
		return;
	char *dst = c->refusal[c->n_refusals];
	int k = snprintf(dst, REAC_SEGCONF_REFUSAL_LEN, "line %d: ", line);
	if (k < 0 || k >= REAC_SEGCONF_REFUSAL_LEN)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(dst + k, (size_t)(REAC_SEGCONF_REFUSAL_LEN - k), fmt, ap);
	va_end(ap);
	c->n_refusals++;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		e--;
	*e = '\0';
	return s;
}

/* `#` or `;` at the start of the line or after whitespace ends it. NOT a bare `;` inside a
 * word: an interface name cannot contain one, but a value could one day, and a comment
 * marker that eats the middle of a token is the kind of surprise a config file must not
 * hold. */
static void strip_comment(char *s)
{
	for (char *p = s; *p; p++) {
		if ((*p == '#' || *p == ';') &&
		    (p == s || p[-1] == ' ' || p[-1] == '\t')) {
			*p = '\0';
			return;
		}
	}
}

/* Unquote a value in place: one matching pair of single or double quotes, and nothing
 * else — no escapes, no expansion. Same deliberate non-shell as reac_conf's env files. */
static char *unquote(char *v)
{
	size_t n = strlen(v);
	if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) {
		v[n - 1] = '\0';
		return v + 1;
	}
	return v;
}

static int parse_bool(const char *v, int *out)
{
	if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0")) {
		*out = 0;
		return 0;
	}
	return -1;
}

/* The entry for `name`, creating it if the table has room. NULL means the table is full,
 * and the caller reports that rather than dropping the section in silence. */
static struct reac_segconf_seg *seg_get(struct reac_segconf *c, const char *name, int *is_new)
{
	*is_new = 0;
	for (int i = 0; i < c->n; i++)
		if (strcmp(c->seg[i].name, name) == 0)
			return &c->seg[i];
	if (c->n >= REAC_SEGCONF_MAX) {
		c->overflow++;
		return NULL;
	}
	struct reac_segconf_seg *s = &c->seg[c->n++];
	memset(s, 0, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", name);
	*is_new = 1;
	return s;
}

int reac_segconf_parse(struct reac_segconf *c, const char *text)
{
	if (!text)
		return c->n;
	struct reac_segconf_seg *cur = NULL;
	char cur_name[IFNAMSIZ] = "";
	const char *p = text;
	int lineno = 0;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char buf[512];
		if (len >= sizeof buf)
			len = sizeof buf - 1;
		memcpy(buf, p, len);
		buf[len] = '\0';
		p = nl ? nl + 1 : p + strlen(p);
		lineno++;

		strip_comment(buf);
		char *line = trim(buf);
		if (!*line)
			continue;

		if (*line == '[') {
			char *close = strchr(line, ']');
			if (!close) {
				refuse(c, lineno, "section is not closed: '%s'", line);
				cur = NULL;
				continue;
			}
			*close = '\0';
			char *body = trim(line + 1);
			/* `[segment <name>]`. The keyword is case-insensitive; the NAME is
			 * not, because the kernel compares interface names byte for byte. */
			if (strncasecmp(body, "segment", 7) != 0 ||
			    (body[7] != ' ' && body[7] != '\t')) {
				refuse(c, lineno, "unknown section [%s] — only [segment <name>] "
				       "is understood", body);
				cur = NULL;
				continue;
			}
			char *name = trim(body + 7);
			if (!*name) {
				refuse(c, lineno, "[segment] names no segment");
				cur = NULL;
				continue;
			}
			if (strlen(name) >= IFNAMSIZ) {
				refuse(c, lineno, "[segment %s] is longer than an interface name "
				       "can be (%d)", name, IFNAMSIZ - 1);
				cur = NULL;
				continue;
			}
			int is_new = 0;
			cur = seg_get(c, name, &is_new);
			if (!cur) {
				refuse(c, lineno, "[segment %s] is past the %d-segment bound and "
				       "was NOT read", name, REAC_SEGCONF_MAX);
				continue;
			}
			if (!is_new)
				refuse(c, lineno, "[segment %s] appears more than once; the keys "
				       "are merged", name);
			snprintf(cur_name, sizeof cur_name, "%s", name);
			continue;
		}

		char *eq = strchr(line, '=');
		if (!eq) {
			refuse(c, lineno, "'%s' is neither a [segment <name>] header nor a "
			       "key = value", line);
			continue;
		}
		*eq = '\0';
		char *key = trim(line);
		char *val = unquote(trim(eq + 1));
		if (!cur) {
			refuse(c, lineno, "'%s' is outside any [segment <name>] section", key);
			continue;
		}
		if (!strcasecmp(key, "role")) {
			enum reac_role_intent i;
			/* THE VALUE FOLDS TOO, and only here. reac_role_intent_parse is the
			 * WIRE/argv vocabulary and stays exact; this is a file a human types,
			 * where `TAP` and `tap` meaning different things would be a trap with
			 * no upside. `ignore`'s yes/no folds for the same reason. */
			char low[64];
			size_t li = 0;
			for (const char *q = val; *q && li + 1 < sizeof low; q++, li++)
				low[li] = (*q >= 'A' && *q <= 'Z') ? (char)(*q - 'A' + 'a') : *q;
			low[li] = '\0';
			if (reac_role_intent_parse(low, &i) != 0) {
				refuse(c, lineno, "[segment %s] role = '%s' is not one of "
				       "auto|master|slave|tap", cur_name, val);
				continue;
			}
			cur->role = i;
			cur->role_set = 1;
		} else if (!strcasecmp(key, "ignore")) {
			int b;
			if (parse_bool(val, &b) != 0) {
				refuse(c, lineno, "[segment %s] ignore = '%s' is not yes|no",
				       cur_name, val);
				continue;
			}
			cur->ignore = b;
		} else {
			refuse(c, lineno, "[segment %s] has no key '%s' — role, ignore",
			       cur_name, key);
		}
	}
	return c->n;
}

int reac_segconf_load(struct reac_segconf *c, const char *home)
{
	char keep[256];
	snprintf(keep, sizeof keep, "%s", home ? home : "");
	reac_segconf_init(c);
	snprintf(c->home, sizeof c->home, "%s", keep);
	if (!home)
		home = getenv("HOME");
	if (!home || !*home)
		home = ".";
	snprintf(c->path, sizeof c->path, "%s/%s/%s", home, REAC_SEGCONF_DIR, REAC_SEGCONF_FILE);

	struct stat st;
	if (stat(c->path, &st) == 0) {
		c->stamp_mtime = (long long)st.st_mtime;
		c->stamp_size  = (long long)st.st_size;
		c->stamp_ino   = (unsigned long long)st.st_ino;
	}
	FILE *f = fopen(c->path, "re");
	if (!f)
		return 0;   /* absent is the normal case, and `present` stays 0 to say which */
	c->present = 1;
	/* Bounded read: this file is a handful of stanzas, and a config reader that will
	 * allocate whatever it is pointed at is a config reader that can be pointed at
	 * /dev/zero. Anything past the bound is REFUSED by name, never truncated quietly. */
	char *text = malloc(64 * 1024);
	if (!text) {
		fclose(f);
		refuse(c, 0, "out of memory reading %s", c->path);
		return 0;
	}
	size_t n = fread(text, 1, 64 * 1024 - 1, f);
	text[n] = '\0';
	int more = (fgetc(f) != EOF);
	fclose(f);
	if (more)
		refuse(c, 0, "%s is larger than 64 KiB; the rest was NOT read", c->path);
	reac_segconf_parse(c, text);
	free(text);
	return c->n;
}

int reac_segconf_refresh(struct reac_segconf *c)
{
	if (!c || !c->path[0])
		return 0;
	struct stat st;
	int there = stat(c->path, &st) == 0;
	long long mt = there ? (long long)st.st_mtime : 0;
	long long sz = there ? (long long)st.st_size : 0;
	unsigned long long ino = there ? (unsigned long long)st.st_ino : 0;
	if (there == (c->present != 0) && mt == c->stamp_mtime &&
	    sz == c->stamp_size && ino == c->stamp_ino)
		return 0;
	char keep[256];
	snprintf(keep, sizeof keep, "%s", c->home);
	reac_segconf_load(c, keep[0] ? keep : NULL);
	return 1;
}

const struct reac_segconf_seg *reac_segconf_find(const struct reac_segconf *c, const char *name)
{
	if (!c || !name)
		return NULL;
	for (int i = 0; i < c->n; i++)
		if (strcmp(c->seg[i].name, name) == 0)
			return &c->seg[i];
	return NULL;
}

int reac_segconf_ignored(const struct reac_segconf *c, const char *name)
{
	const struct reac_segconf_seg *s = reac_segconf_find(c, name);
	return s && s->ignore;
}

int reac_segconf_role(const struct reac_segconf *c, const char *name, enum reac_role_intent *out)
{
	const struct reac_segconf_seg *s = reac_segconf_find(c, name);
	if (!s || !s->role_set)
		return 0;
	if (out)
		*out = s->role;
	return 1;
}

int reac_segconf_declared(const struct reac_segconf *c, struct reac_declared_vlan *tab,
                          int max, int *n)
{
	if (!c)
		return 0;
	int added = 0;
	for (int i = 0; i < c->n; i++) {
		if (c->seg[i].ignore)
			continue;   /* switching a segment off cannot create a netdev for it */
		char parent[IFNAMSIZ];
		uint16_t vid;
		if (!reac_declared_vlan_split(c->seg[i].name, parent, sizeof parent, &vid))
			continue;   /* not a VLAN name: an ordinary NIC declares nothing */
		int r = reac_declared_vlan_add(tab, max, n, parent, vid);
		if (r < 0)
			return -1;
		added += r;
	}
	return added;
}
