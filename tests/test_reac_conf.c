// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The precedence LAW, pinned. docs/RATE-AND-CLOCK-CONFIG.md declares an order;
 * this is what stops the declaration and the implementation drifting apart, and
 * it is the reason the law is worth writing down at all. An override chain whose
 * order is only in prose is a chain every reader infers differently.
 *
 * Each case puts the SAME key in two layers with different values and requires
 * the higher one to win — a test that only checked "the value is found" would
 * pass against any order at all. */

#include <reac/transport/reac_conf.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char home[512];

static void wr(const char *rel, const char *body)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", home, rel);
	/* mkdir -p over the parent */
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		char acc[1024] = "";
		for (char *p = dir; *p; ) {
			char *n = strchr(p + 1, '/');
			size_t len = n ? (size_t)(n - dir) : strlen(dir);
			memcpy(acc, dir, len);
			acc[len] = '\0';
			mkdir(acc, 0700);
			if (!n)
				break;
			p = n;
		}
	}
	FILE *f = fopen(path, "we");
	assert(f);
	fputs(body, f);
	fclose(f);
}

static void rm(const char *rel)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", home, rel);
	unlink(path);
}

#define HOST ".config/reac-pw/reac-pw.env"
#define LAST ".config/openmixer/reac.env"

static enum reac_conf_layer look(char *out, size_t cap)
{
	return reac_conf_lookup("REAC_RATE", "eth0", home, out, cap);
}

int main(void)
{
	char tmpl[] = "/tmp/reac_conf_testXXXXXX";
	assert(mkdtemp(tmpl));
	snprintf(home, sizeof home, "%s", tmpl);
	unsetenv("REAC_RATE");

	char v[64];

	/* Nothing anywhere: the caller's built-in must be reached, and `out` must be
	 * left alone so a caller that pre-seeded it is not clobbered. */
	snprintf(v, sizeof v, "UNTOUCHED");
	assert(look(v, sizeof v) == REAC_CONF_NONE);
	assert(!strcmp(v, "UNTOUCHED"));

	/* Layer 5 alone — the last resort answers when nothing above does. This is
	 * the standalone-install case and the whole reason the file stays. */
	wr(LAST, "# the floor\nREAC_RATE=96000\n");
	assert(look(v, sizeof v) == REAC_CONF_LAST_RESORT);
	assert(!strcmp(v, "96000"));

	/* Layer 4 over 5. */
	wr(HOST, "REAC_RATE=48000\n");
	assert(look(v, sizeof v) == REAC_CONF_HOST);
	assert(!strcmp(v, "48000"));

	/* Layer 3 over 4 over 5 — the per-segment KEY is what a two-segment rig
	 * needs, and it must beat the bare key in the same file. */
	wr(HOST, "REAC_RATE=48000\nREAC_RATE_eth0=44100\n");
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "44100"));

	/* The per-segment key beats the bare key EVEN IN THE ENVIRONMENT: systemd's
	 * EnvironmentFile= exports the whole of reac-pw.env, so a bare REAC_RATE in
	 * the environment is the same file speaking, not an operator overriding it. */
	setenv("REAC_RATE", "96000", 1);
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "44100"));
	setenv("REAC_RATE_eth0", "96000", 1);
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "96000"));
	unsetenv("REAC_RATE_eth0");
	unsetenv("REAC_RATE");
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "44100"));

	/* Layer 2 over everything below it, for a segment with no key of its own. */
	setenv("REAC_RATE", "96000", 1);
	assert(reac_conf_lookup("REAC_RATE", "eth9", home, v, sizeof v) == REAC_CONF_ENV);
	assert(!strcmp(v, "96000"));
	unsetenv("REAC_RATE");

	/* A per-segment key for a DIFFERENT segment must not answer. The two
	 * segments on this rig are not interchangeable and neither are their keys. */
	assert(reac_conf_lookup("REAC_RATE", "eth9", home, v, sizeof v) == REAC_CONF_HOST);
	assert(!strcmp(v, "48000"));
	/* ...and with no segment at all, layer 3 is skipped rather than guessed. */
	assert(reac_conf_lookup("REAC_RATE", NULL, home, v, sizeof v) == REAC_CONF_HOST);

	/* An EMPTY value is not an answer: a key someone blanked out is a key they
	 * turned off. It must fall THROUGH to the next layer, not return "". */
	wr(HOST, "REAC_RATE=48000\nREAC_RATE_eth0=\n");
	assert(look(v, sizeof v) == REAC_CONF_HOST);
	assert(!strcmp(v, "48000"));
	setenv("REAC_RATE", "", 1);
	assert(look(v, sizeof v) == REAC_CONF_HOST);
	unsetenv("REAC_RATE");

	/* Comments, blanks, quotes, `export`, and surrounding whitespace. */
	wr(HOST, "\n # REAC_RATE_eth0=11111\n\n  export  REAC_RATE_eth0 = \"96000\"  \n");
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "96000"));
	wr(HOST, "REAC_RATE_eth0='44100'\n");
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "44100"));

	/* A key that merely STARTS with ours must not match — neither the bare one
	 * nor the suffixed one. */
	wr(HOST, "REAC_RATE_OVERRIDE=11111\nREAC_RATE_eth0_OVERRIDE=22222\n");
	assert(look(v, sizeof v) == REAC_CONF_LAST_RESORT);

	/* Last assignment in a file wins, as a shell and EnvironmentFile both do. */
	wr(HOST, "REAC_RATE_eth0=44100\nREAC_RATE_eth0=96000\n");
	assert(look(v, sizeof v) == REAC_CONF_SEGMENT);
	assert(!strcmp(v, "96000"));

	/* An unreadable/absent layer is a MISS, not a failure that hides the ones
	 * below it. Removing the top file must expose the next, not return NONE. */
	rm(HOST);
	assert(look(v, sizeof v) == REAC_CONF_LAST_RESORT);
	rm(LAST);
	assert(look(v, sizeof v) == REAC_CONF_NONE);

	/* Every layer names itself, and no name is NULL or empty — this string goes
	 * in front of an operator. */
	for (int l = REAC_CONF_NONE; l <= REAC_CONF_BUILTIN; l++) {
		const char *n = reac_conf_layer_name((enum reac_conf_layer)l);
		assert(n && *n);
	}

	char cmd[1024];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not clean %s\n", tmpl);

	printf("test_reac_conf: OK\n");
	return 0;
}
