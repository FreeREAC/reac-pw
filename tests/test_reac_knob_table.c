// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* "We should be able to set them and keep them if needed, and announce them when
 * detected, so that we can manage them; autodetection does not mean obscurity, it's
 * discovery and publish." (operator, 2026-09-17). This pins:
 *
 *  1. every table entry names WHY it is not conf_capable, when it is not (a bare
 *     `0` with no reason is exactly the obscurity the ruling refuses);
 *  2. reac_conf_flag reads through the SAME layering as reac_conf_lookup, with the
 *     right default when nothing answers;
 *  3. reac_knobs_announce prints the token FIRST, the grammar the ruling specifies,
 *     nothing for an unset knob, and a summary whose counts add up to the table;
 *  4. the CODE <-> DOC round trip: every table key is documented in
 *     docs/ENV-KNOBS.md, and every REACPW_/REAC_ knob documented there (except the
 *     retired REAC_ROLE, which this table deliberately excludes) is in the table —
 *     so the two cannot drift the way the old hand-kept promise never checked;
 *  5. --set KEY=VALUE (operator ruling, 2026-09-17): cli beats env, VALUE checked
 *     (not presence), the last --set for a key wins, and an unknown key is refused. */

#include "reac_knobs.h"
#include "reac_code.h"

#include <reac/transport/reac_conf.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

#define CHECK(cond, ...)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL: ");                                  \
			printf(__VA_ARGS__);                                  \
			printf("\n");                                         \
			fails++;                                              \
		}                                                             \
	} while (0)

int main(void)
{
	/* HERMETIC HOME. reac_conf_lookup falls back to $HOME for its file layers
	 * (~/.config/reac-pw/reac-pw.env, ~/.config/openmixer/reac.env) when no
	 * caller-supplied `home` is given, and reac_knobs_announce has no way to
	 * override it -- it is meant to read the REAL operator config at start. A
	 * test run on the desk's own account would otherwise see the desk's real
	 * knobs and report false extras, which is what the first run of this test
	 * did (3 "set" that were really this machine's reac-pw.env). Point HOME at
	 * an empty scratch dir for the whole run instead of asserting against
	 * whatever config happens to exist here today. */
	char homedir[] = "/tmp/reac_knob_table_test_home_XXXXXX";
	CHECK(mkdtemp(homedir) != NULL, "mkdtemp for a scratch HOME failed");
	const char *real_home = getenv("HOME");
	char real_home_buf[1024] = "";
	if (real_home)
		snprintf(real_home_buf, sizeof real_home_buf, "%s", real_home);
	setenv("HOME", homedir, 1);

	/* ---- 1. every entry is well-formed, and a knob that is NOT conf_capable
	 * says why -- the table itself must not be the new obscurity. */
	for (int i = 0; i < g_reac_knobs_count; i++) {
		const struct reac_knob *k = &g_reac_knobs[i];
		CHECK(k->key && *k->key, "table entry %d has an empty key", i);
		if (!k->conf_capable)
			CHECK(k->why_not_conf && *k->why_not_conf,
			      "%s is not conf_capable but names no reason", k->key);
		else
			CHECK(k->why_not_conf == NULL,
			      "%s is conf_capable but still carries a reason", k->key);
	}
	CHECK(g_reac_knobs_count >= 20,
	      "found only %d knobs -- the table is broken, not the daemon "
	      "(expected the ~24 enumerated 2026-09-17)", g_reac_knobs_count);

	/* ---- 2. reac_conf_flag: layered, and the right default on every miss. */
	{
		const char *key = "REACPW_TEST_KNOB_FLAG";
		unsetenv(key);
		CHECK(reac_conf_flag(key, 1) == 1, "unset should fall back to dflt=1");
		CHECK(reac_conf_flag(key, 0) == 0, "unset should fall back to dflt=0");
		setenv(key, "0", 1);
		CHECK(reac_conf_flag(key, 1) == 0, "explicit 0 must override dflt=1");
		setenv(key, "yes", 1);
		CHECK(reac_conf_flag(key, 0) == 1, "explicit 'yes' must override dflt=0");
		setenv(key, "not-a-boolean", 1);
		CHECK(reac_conf_flag(key, 1) == 1,
		      "an unparseable value must fall back to dflt, not read as true");
		unsetenv(key);
	}

	/* ---- 3. the announce grammar, red against a known knob. */
	{
		const char *key = "REAC_DEBUG";
		setenv(key, "1", 1);

		char tmpl[] = "/tmp/reac_knob_table_test_XXXXXX";
		int fd = mkstemp(tmpl);
		CHECK(fd >= 0, "mkstemp failed");
		FILE *f = fdopen(fd, "w+");
		CHECK(f != NULL, "fdopen failed");
		if (f) {
			int set = reac_knobs_announce(f);
			fflush(f);
			rewind(f);
			char buf[65536];
			size_t n = fread(buf, 1, sizeof buf - 1, f);
			buf[n] = '\0';
			CHECK(strstr(buf, "reac-pw: S_KNOB_SET knob REAC_DEBUG=1 (env)\n") != NULL,
			      "the exact grammar line is missing; got:\n%s", buf);
			CHECK(strstr(buf, "reac-pw: S_KNOB_SUMMARY knobs: ") != NULL,
			      "no summary line; got:\n%s", buf);
			char want_summary[128];
			int unset = g_reac_knobs_count - set;
			snprintf(want_summary, sizeof want_summary,
			         "knobs: %d set, %d default\n", set, unset);
			CHECK(strstr(buf, want_summary) != NULL,
			      "summary counts don't add up to the table (%s); got:\n%s",
			      want_summary, buf);
			CHECK(set >= 1, "REAC_DEBUG=1 was set but announce found nothing set");
			fclose(f);
		}
		unsetenv(key);
		remove(tmpl);
	}

	/* ---- 3b. silence is the whole story of a default: nothing set -> nothing
	 * but the summary line. */
	{
		for (int i = 0; i < g_reac_knobs_count; i++)
			unsetenv(g_reac_knobs[i].key);

		char tmpl[] = "/tmp/reac_knob_table_test_XXXXXX";
		int fd = mkstemp(tmpl);
		FILE *f = fdopen(fd, "w+");
		if (f) {
			int set = reac_knobs_announce(f);
			fflush(f);
			rewind(f);
			char buf[8192];
			size_t n = fread(buf, 1, sizeof buf - 1, f);
			buf[n] = '\0';
			CHECK(set == 0, "expected 0 knobs set with everything unset, got %d", set);
			char want[64];
			snprintf(want, sizeof want, "knobs: 0 set, %d default\n", g_reac_knobs_count);
			CHECK(strstr(buf, want) != NULL,
			      "expected only the summary line; got:\n%s", buf);
			fclose(f);
		}
		remove(tmpl);
	}

	/* ---- 4. CODE <-> DOC round trip. */
	{
		const char *root = getenv("REACPW_SRCDIR");
		char path[1024];
		snprintf(path, sizeof path, "%s/docs/ENV-KNOBS.md", root ? root : ".");
		FILE *f = fopen(path, "r");
		if (!f) {
			printf("  ENV-KNOBS.md not readable at %s — NOT CHECKED\n", path);
			fails++;
		} else {
			char *doc = NULL;
			size_t cap = 0, len = 0;
			char line[4096];
			while (fgets(line, sizeof line, f)) {
				size_t l = strlen(line);
				if (len + l + 1 > cap) {
					cap = (cap ? cap * 2 : 8192) + l;
					doc = realloc(doc, cap);
				}
				memcpy(doc + len, line, l);
				len += l;
				doc[len] = '\0';
			}
			fclose(f);

			/* code -> doc: every table key appears as `KEY=` somewhere. */
			for (int i = 0; i < g_reac_knobs_count; i++) {
				char needle[128];
				snprintf(needle, sizeof needle, "%s=", g_reac_knobs[i].key);
				CHECK(strstr(doc, needle) != NULL,
				      "docs/ENV-KNOBS.md does not document %s", g_reac_knobs[i].key);
			}

			/* doc -> code: every `REACPW_..=`/`REAC_..=` token right after an
			 * opening backtick is in g_reac_knobs, except the retired
			 * REAC_ROLE (this table deliberately excludes it -- it decides
			 * nothing at start, named elsewhere). A KEY is an uppercase/digit/
			 * underscore run starting with REAC immediately after a '`', ending
			 * at the first character that is neither -- checked ONLY when that
			 * character is '=' (a KEY= form); anything else (a bare word, a
			 * `REAC_ROLE` / `REAC_ROLE_<segment>` prose form) is not a knob
			 * assignment and is skipped. */
			for (const char *p = doc; *p; p++) {
				if (*p != '`' || strncmp(p + 1, "REAC", 4) != 0)
					continue;
				const char *start = p + 1;
				const char *q = start;
				while (*q && (isupper((unsigned char)*q) || isdigit((unsigned char)*q) ||
				              *q == '_'))
					q++;
				if (*q != '=')
					continue;
				size_t klen = (size_t)(q - start);
				if (klen == 0 || klen >= 64)
					continue;
				char key[64];
				memcpy(key, start, klen);
				key[klen] = '\0';
				if (strcmp(key, "REAC_ROLE") == 0)
					continue;
				int found = 0;
				for (int i = 0; i < g_reac_knobs_count; i++)
					if (strcmp(g_reac_knobs[i].key, key) == 0) { found = 1; break; }
				CHECK(found, "docs/ENV-KNOBS.md documents %s but g_reac_knobs does not "
				      "read it", key);
			}
			free(doc);
		}
	}

	/* ---- 5. THE COMMAND LINE: highest precedence, over env, VALUE asserted (not
	 * presence) — an unknown key is refused, not silently ignored (operator
	 * ruling, 2026-09-17). LAST: reac_knobs_set_argv has no reset, so once this
	 * runs, REAC_DEBUG is pinned to a cli value for the rest of the process —
	 * every earlier section (the announce grammar, the doc round trip) depends on
	 * REAC_DEBUG being freely settable via env/unset, so this must not run before
	 * them. Uses REAC_DEBUG: a real table entry, exercising the exact resolver
	 * push_libreac_tunables() and reac_knobs_announce() use, not a parallel one. */
	{
		const char *key = "REAC_DEBUG";
		char v[64];

		CHECK(reac_knobs_set_argv("REAC_NOT_A_REAL_KNOB", "1") == 0,
		      "an unknown --set key must be refused, not silently accepted");

		setenv(key, "env-value", 1);
		CHECK(reac_knobs_resolve(key, v, sizeof v) == REAC_CONF_ENV &&
		      strcmp(v, "env-value") == 0,
		      "env alone should answer 'env-value', got layer/value mismatch");

		CHECK(reac_knobs_set_argv(key, "cli-value") == 1,
		      "a known --set key must be accepted");
		CHECK(reac_knobs_resolve(key, v, sizeof v) == REAC_CONF_ARGV &&
		      strcmp(v, "cli-value") == 0,
		      "cli must beat env: expected 'cli-value' (REAC_CONF_ARGV), got '%s'", v);

		CHECK(reac_knobs_set_argv(key, "cli-value-2") == 1,
		      "a second --set for the same key must replace the first");
		CHECK(reac_knobs_resolve(key, v, sizeof v) == REAC_CONF_ARGV &&
		      strcmp(v, "cli-value-2") == 0,
		      "the LAST --set for a key must win, got '%s'", v);

		unsetenv(key);
	}

	if (real_home_buf[0])
		setenv("HOME", real_home_buf, 1);
	else
		unsetenv("HOME");
	rmdir(homedir);

	if (fails) {
		printf("test_reac_knob_table: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_knob_table: table is well-formed, reac_conf_flag is layered, "
	       "the announce grammar is exact, silence is silent, and the code<->doc "
	       "round trip holds for %d knobs\n", g_reac_knobs_count);
	return 0;
}
