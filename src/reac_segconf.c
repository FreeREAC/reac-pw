// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_segconf.h"

#include <reac/reac_ctrlblk.h>   /* the box-model table: a model token is a ROW, never a width */
#include "reac_declared_vlan.h"

#include <dirent.h>
#include <errno.h>
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
	/* THE FILE IS PART OF THE ADDRESS NOW. With a directory behind the conf a bare line
	 * number points at nothing, and the operator's next act is to open the wrong file. */
	const char *where = c->reading[0] ? c->reading : REAC_SEGCONF_FILE;
	int k = line > 0 ? snprintf(dst, REAC_SEGCONF_REFUSAL_LEN, "%s:%d: ", where, line)
	                 : snprintf(dst, REAC_SEGCONF_REFUSAL_LEN, "%s: ", where);
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

int reac_segconf_parse_file(struct reac_segconf *c, const char *text, const char *label)
{
	snprintf(c->reading, sizeof c->reading, "%s", label ? label : REAC_SEGCONF_FILE);
	c->gen++;
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
			/* A REPEATED HEADER IS A TYPO WITHIN ONE FILE AND AN OVERRIDE ACROSS
			 * TWO. Same text, two meanings, told apart by the only thing that can
			 * tell them apart: which file we are in. Refusing the override would
			 * refuse the drop-in directory's whole purpose. */
			if (!is_new && cur->seen_gen == c->gen)
				refuse(c, lineno, "[segment %s] appears more than once in this "
				       "file; the keys are merged", name);
			cur->seen_gen = c->gen;
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
				       "auto|master|slave|tap|box", cur_name, val);
				continue;
			}
			cur->role = i;
			cur->role_set = 1;
			snprintf(cur->role_file, sizeof cur->role_file, "%s", c->reading);
		} else if (!strcasecmp(key, "model")) {
			/* Folded like `role`'s value and for the same reason: this is a file
			 * a human types. The token is looked up in libreac's table NOW, so an
			 * unknown model is named at the line that holds it and never becomes
			 * a width nobody declared. */
			char low[REAC_SEGCONF_MODEL_LEN];
			size_t li = 0;
			for (const char *q = val; *q && li + 1 < sizeof low; q++, li++)
				low[li] = (*q >= 'A' && *q <= 'Z') ? (char)(*q - 'A' + 'a') : *q;
			low[li] = '\0';
			if (!reac_box_model_by_token(low)) {
				char known[240];
				size_t kn = 0, ntok = 0;
				const struct reac_box_model *tab = reac_box_model_table(&ntok);
				for (size_t ti = 0; ti < ntok && kn + 2 < sizeof known; ti++)
					kn += (size_t)snprintf(known + kn, sizeof known - kn, "%s%s",
					                       ti ? "|" : "", tab[ti].token);
				refuse(c, lineno, "[segment %s] model = '%s' is not a box model — "
				       "%s", cur_name, val, known);
				continue;
			}
			snprintf(cur->model, sizeof cur->model, "%s", low);
			cur->model_set = 1;
			snprintf(cur->model_file, sizeof cur->model_file, "%s", c->reading);
		} else if (!strcasecmp(key, "ignore")) {
			int b;
			if (parse_bool(val, &b) != 0) {
				refuse(c, lineno, "[segment %s] ignore = '%s' is not yes|no",
				       cur_name, val);
				continue;
			}
			cur->ignore = b;
			cur->ignore_set = 1;
			snprintf(cur->ignore_file, sizeof cur->ignore_file, "%s", c->reading);
		} else {
			refuse(c, lineno, "[segment %s] has no key '%s' — role, model, ignore",
			       cur_name, key);
		}
	}
	return c->n;
}

/* WHAT ONE KEY MEANS DEPENDS ON ANOTHER, so it is checked once EVERY FILE HAS BEEN READ
 * and never at the line. `model` may be written above `role`, and a drop-in may supply the
 * role for a model the hand-written file declared — checking at the line would refuse a
 * file that is correct by the time the last one has been read (spec amendment §A: last
 * wins per key). Both refusals name the segment, and neither is fatal. */
static void cross_check(struct reac_segconf *c)
{
	for (int i = 0; i < c->n; i++) {
		struct reac_segconf_seg *s = &c->seg[i];
		int is_box = s->role_set && s->role == REAC_ROLE_INTENT_BOX;
		if (s->model_set && !is_box) {
			refuse(c, 0, "[segment %s] model = '%s' is meaningful only under "
			       "role = box; the role stands and the model is dropped",
			       s->name, s->model);
			s->model_set = 0;
			s->model[0] = '\0';
		} else if (is_box && !s->model_set) {
			/* NO DEFAULT MODEL, on purpose: presenting an arbitrary width to a
			 * mixer patches somebody's inputs onto the wrong channels, and an
			 * absence is a fact. The segment falls back to auto, which is what it
			 * would have been with nothing said. */
			refuse(c, 0, "[segment %s] role = box needs a model — none was given, "
			       "so the segment falls back to auto", s->name);
			s->role_set = 0;
			s->role = REAC_ROLE_INTENT_AUTO;
		}
	}
}

int reac_segconf_parse(struct reac_segconf *c, const char *text)
{
	int n = reac_segconf_parse_file(c, text, REAC_SEGCONF_FILE);
	cross_check(c);
	return n;
}

/* One file's whole text, parsed as `label`. Returns 1 if it was read, 0 if it was not
 * there, -1 if it was there and could not be read (a refusal, never fatal). The stamp is
 * taken WHATEVER the answer, because "it was not there" is a state the refresh must be
 * able to see change. */
static int read_one(struct reac_segconf *c, const char *label, struct reac_segconf_stamp *st)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", c->base, label);
	struct stat sb;
	memset(st, 0, sizeof *st);
	if (stat(path, &sb) == 0) {
		st->present  = 1;
		st->mtime_ns = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec;
		st->size     = (long long)sb.st_size;
		st->ino      = (unsigned long long)sb.st_ino;
	}
	FILE *f = fopen(path, "re");
	if (!f) {
		/* A DANGLING SYMLINK AND A PERMISSION DENIAL ARE NOT "ABSENT". The caller
		 * decides which of those two this is: the hand-written file's absence is the
		 * normal case, a drop-in that the DIRECTORY listed and we cannot open is a
		 * fact the operator needs. */
		return st->present ? -1 : 0;
	}
	if (c->n_files < REAC_SEGCONF_FILES)
		snprintf(c->file[c->n_files++], REAC_SEGCONF_FILE_LEN, "%s", label);
	/* Bounded read: this file is a handful of stanzas, and a config reader that will
	 * allocate whatever it is pointed at is a config reader that can be pointed at
	 * /dev/zero. Anything past the bound is REFUSED by name, never truncated quietly. */
	char *text = malloc(64 * 1024);
	if (!text) {
		fclose(f);
		snprintf(c->reading, sizeof c->reading, "%s", label);
		refuse(c, 0, "out of memory reading this file");
		return -1;
	}
	size_t n = fread(text, 1, 64 * 1024 - 1, f);
	text[n] = '\0';
	int more = (fgetc(f) != EOF);
	fclose(f);
	snprintf(c->reading, sizeof c->reading, "%s", label);
	if (more)
		refuse(c, 0, "larger than 64 KiB; the rest was NOT read");
	reac_segconf_parse_file(c, text, label);
	free(text);
	return 1;
}

/* The drop-ins, in BYTE order of their name. Not locale collation: a configuration whose
 * precedence changes with $LANG is not a precedence. */
static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void load_dropins(struct reac_segconf *c)
{
	char dird[1024];
	snprintf(dird, sizeof dird, "%s/%s", c->base, REAC_SEGCONF_DIRD);
	struct stat sb;
	memset(&c->stamp_dir, 0, sizeof c->stamp_dir);
	if (stat(dird, &sb) == 0) {
		c->stamp_dir.present  = 1;
		c->stamp_dir.mtime_ns = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec;
		c->stamp_dir.size     = (long long)sb.st_size;
		c->stamp_dir.ino      = (unsigned long long)sb.st_ino;
	}
	DIR *d = opendir(dird);
	if (!d)
		return;   /* no directory is the normal case, exactly like no conf */
	/* One name is one drop-in; the bound is the same class as every other bound here and
	 * the excess is COUNTED, not dropped in silence. */
	static char names[REAC_SEGCONF_FILES][REAC_SEGCONF_FILE_LEN];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		const char *nm = e->d_name;
		size_t len = strlen(nm);
		/* `*.conf`, and nothing hidden: an editor's `.#50-openmixer.conf` swap file
		 * and a `.bak` are not configuration, and reading either is how a half-written
		 * file becomes a rig. */
		if (nm[0] == '.' || len < 6 || strcmp(nm + len - 5, ".conf") != 0)
			continue;
		if (len + sizeof(REAC_SEGCONF_DIRD) + 1 >= REAC_SEGCONF_FILE_LEN) {
			c->files_overflow++;   /* a name we could not even print in a refusal */
			continue;
		}
		if (n >= REAC_SEGCONF_FILES - 1) {
			c->files_overflow++;
			continue;
		}
		snprintf(names[n++], REAC_SEGCONF_FILE_LEN, "%s", nm);
	}
	closedir(d);
	qsort(names, (size_t)n, REAC_SEGCONF_FILE_LEN, name_cmp);
	for (int i = 0; i < n; i++) {
		char label[REAC_SEGCONF_FILE_LEN];
		snprintf(label, sizeof label, "%s/%s", REAC_SEGCONF_DIRD, names[i]);
		int slot = c->n_files;
		if (slot >= REAC_SEGCONF_FILES) {
			c->files_overflow++;   /* the stamp table is full; never clobber one */
			continue;
		}
		int r = read_one(c, label, &c->stamp[slot]);
		/* THE DIRECTORY LISTED IT, so "not there" is not the normal case here — it is
		 * a dangling symlink, a permission denial, or a file removed under us. Each of
		 * those is a fact the operator needs; only the hand-written file is allowed to
		 * be absent in silence. */
		if (r <= 0) {
			snprintf(c->reading, sizeof c->reading, "%s", label);
			refuse(c, 0, "is listed in the directory and could NOT be read (%s); "
			       "the files that could be read are still applied", strerror(errno));
		}
	}
	if (c->files_overflow)
		refuse(c, 0, "%u drop-in(s) past the %d-file bound were NOT read",
		       c->files_overflow, REAC_SEGCONF_FILES - 1);
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
	snprintf(c->base, sizeof c->base, "%s/%s", home, REAC_SEGCONF_DIR);
	snprintf(c->path, sizeof c->path, "%s/%s", c->base, REAC_SEGCONF_FILE);

	/* THE HAND-WRITTEN FILE FIRST, and the drop-ins after it: later wins per key
	 * (reac_segconf.h's REAC_SEGCONF_DIRD has the four reasons). An absent hand-written
	 * file is the normal case and `present` is what says so — "absent" and "empty" must
	 * not read alike. */
	if (read_one(c, REAC_SEGCONF_FILE, &c->stamp[0]) > 0)
		c->present = 1;
	load_dropins(c);
	/* EVERY FILE HAS BEEN READ NOW, which is the only moment the cross-key rules can
	 * be judged: a drop-in may supply the role for a model the hand-written file
	 * declared, or the other way round. */
	cross_check(c);
	return c->n;
}

/* Has `label` moved since the stamp? An absent file with an absent stamp has not. */
static int moved(const struct reac_segconf *c, const char *label,
                 const struct reac_segconf_stamp *st)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", c->base, label);
	struct stat sb;
	int there = stat(path, &sb) == 0;
	if (there != st->present)
		return 1;
	if (!there)
		return 0;
	long long mt = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec;
	return mt != st->mtime_ns || (long long)sb.st_size != st->size ||
	       (unsigned long long)sb.st_ino != st->ino;
}

int reac_segconf_refresh(struct reac_segconf *c)
{
	if (!c || !c->base[0])
		return 0;
	int move = moved(c, REAC_SEGCONF_FILE, &c->stamp[0]) ||
	           moved(c, REAC_SEGCONF_DIRD, &c->stamp_dir);
	/* Only if neither moved: the files the directory listed last time. A file REWRITTEN
	 * in place does not move the directory, which is exactly what a console rewriting
	 * its own drop-in does. */
	for (int i = 0; !move && i < c->n_files; i++)
		if (strcmp(c->file[i], REAC_SEGCONF_FILE) != 0)
			move = moved(c, c->file[i], &c->stamp[i]);
	if (!move)
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

const char *reac_segconf_role_file(const struct reac_segconf *c, const char *name)
{
	const struct reac_segconf_seg *s = reac_segconf_find(c, name);
	return s && s->role_set && s->role_file[0] ? s->role_file : NULL;
}

const char *reac_segconf_ignore_file(const struct reac_segconf *c, const char *name)
{
	const struct reac_segconf_seg *s = reac_segconf_find(c, name);
	return s && s->ignore_set && s->ignore_file[0] ? s->ignore_file : NULL;
}

const char *reac_segconf_model(const struct reac_segconf *c, const char *name)
{
	const struct reac_segconf_seg *s = reac_segconf_find(c, name);
	return s && s->model_set ? s->model : NULL;
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
