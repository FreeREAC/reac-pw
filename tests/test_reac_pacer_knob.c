// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REACPW_PACER — the knob that chooses which backend owns the egress instant.
 *
 * WHAT THIS PINS, AND WHAT IT DELIBERATELY DOES NOT. The mapping from the word to
 * the backend, and the refusal when a precondition is missing, live in
 * libreac-transport and are tested there (`test_reac_etf`, `make test`). What is
 * reac-pw's own contract is the part an OPERATOR touches: the knob's NAME, the fact
 * that it rides the same layered precedence as every other knob — so a segment can
 * be put on one arm and its neighbour on the other for a comparison — and that
 * silence means the thread backend, which is what has always run.
 *
 * That last one is the reason this file exists. The comparative run puts one
 * segment on `etf` and one on `thread`; if the per-segment layer did not work for
 * this key, both segments would quietly run the same backend and the table would
 * print two columns of the same measurement. Nothing else would say so.
 *
 * It is the layer lookup that is asserted, not a pacer: opening a pacer needs a NIC
 * and CAP_NET_RAW, and neither belongs in a unit test. */

#include <reac/transport/reac_conf.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEY "REACPW_PACER"
#define LEAD_KEY "REACPW_PACER_LEAD_US"

static char home[512];
static int fails;

#define CHECK(cond, ...) do { \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	               printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void wr(const char *rel, const char *body)
{
	char path[1024], dir[1024];
	snprintf(path, sizeof path, "%s/%s", home, rel);
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
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	snprintf(home, sizeof home, "/tmp/reacpw-knob-%d", (int)getpid());
	mkdir(home, 0700);

	char v[64];
	enum reac_conf_layer l;

	/* ---- 1. SILENCE IS THE THREAD BACKEND ---------------------------------- *
	 * Not "etf", not "whatever was last set". A daemon that changed how it paces
	 * because a file appeared somewhere would be a daemon nobody could reason
	 * about; the caller's built-in default is reached only when every layer is
	 * quiet, and that default is the pacer that has always run. */
	unsetenv(KEY);
	l = reac_conf_lookup(KEY, "enp131s0.11", home, v, sizeof v);
	CHECK(l == REAC_CONF_NONE, "an unset %s answered from layer %d (%s)", KEY,
	      (int)l, reac_conf_layer_name(l));

	/* An EMPTY value is a key someone turned off, not a key set to "" — the same
	 * rule every other knob follows. It must fall through to the default, never be
	 * taken as a backend name. */
	setenv(KEY, "", 1);
	l = reac_conf_lookup(KEY, "enp131s0.11", home, v, sizeof v);
	CHECK(l == REAC_CONF_NONE, "an EMPTY %s answered from layer %d (%s)", KEY,
	      (int)l, reac_conf_layer_name(l));
	unsetenv(KEY);

	/* ---- 2. THE PER-SEGMENT LAYER, which the comparison depends on ---------- *
	 * One segment on each arm, in one process, at one time. Without this the
	 * comparative table would print two columns measuring the same backend. */
	setenv(KEY, "thread", 1);
	setenv(KEY "_enp131s0.12", "etf", 1);
	l = reac_conf_lookup(KEY, "enp131s0.12", home, v, sizeof v);
	CHECK(l == REAC_CONF_SEGMENT && !strcmp(v, "etf"),
	      "the per-segment key lost to the bare key: layer %s value '%s'",
	      reac_conf_layer_name(l), v);
	/* ...and the OTHER segment, in the same environment, still reads the bare key.
	 * Asserting only the first would pass against an implementation that returned
	 * the segment value for everybody. */
	l = reac_conf_lookup(KEY, "enp131s0.11", home, v, sizeof v);
	CHECK(l == REAC_CONF_ENV && !strcmp(v, "thread"),
	      "the neighbouring segment picked up .12's value: layer %s value '%s'",
	      reac_conf_layer_name(l), v);
	unsetenv(KEY "_enp131s0.12");
	unsetenv(KEY);

	/* ---- 3. THE ENVIRONMENT OUTRANKS THE FILES ----------------------------- *
	 * The operator's one-run override has to beat the host file, or a comparative
	 * run cannot be set up without editing (and then remembering to restore) a
	 * file that describes every run. */
	wr(".config/reac-pw/reac-pw.env", "REACPW_PACER=thread\n");
	setenv(KEY, "etf", 1);
	l = reac_conf_lookup(KEY, NULL, home, v, sizeof v);
	CHECK(l == REAC_CONF_ENV && !strcmp(v, "etf"),
	      "the host file beat the environment: layer %s value '%s'",
	      reac_conf_layer_name(l), v);
	unsetenv(KEY);
	/* With the environment quiet the file answers — the control for the case
	 * above, which would otherwise pass against a lookup that ignored files
	 * entirely. */
	l = reac_conf_lookup(KEY, NULL, home, v, sizeof v);
	CHECK(l == REAC_CONF_HOST && !strcmp(v, "thread"),
	      "the host file did not answer with the environment quiet: layer %s "
	      "value '%s'", reac_conf_layer_name(l), v);

	/* ---- 4. THE LEAD RIDES THE SAME LAYERS --------------------------------- */
	setenv(LEAD_KEY, "1200", 1);
	l = reac_conf_lookup(LEAD_KEY, NULL, home, v, sizeof v);
	CHECK(l == REAC_CONF_ENV && !strcmp(v, "1200"),
	      "%s did not resolve from the environment: layer %s value '%s'",
	      LEAD_KEY, reac_conf_layer_name(l), v);
	unsetenv(LEAD_KEY);
	l = reac_conf_lookup(LEAD_KEY, NULL, home, v, sizeof v);
	CHECK(l == REAC_CONF_NONE, "%s answered with nothing set (layer %s)", LEAD_KEY,
	      reac_conf_layer_name(l));

	/* ---- 5. THE KNOB IS DOCUMENTED ----------------------------------------- *
	 * A knob an operator cannot find is a knob that does not exist. ENV-KNOBS.md
	 * is where they look, and this is what stops the two drifting. */
	{
		const char *root = getenv("REACPW_SRCDIR");
		char path[1024];
		snprintf(path, sizeof path, "%s/docs/ENV-KNOBS.md", root ? root : ".");
		FILE *f = fopen(path, "r");
		if (!f) {
			printf("  ENV-KNOBS.md not readable at %s — NOT CHECKED\n", path);
			fails++;
		} else {
			char line[4096];
			int saw_pacer = 0, saw_lead = 0;
			while (fgets(line, sizeof line, f)) {
				if (strstr(line, KEY "="))
					saw_pacer = 1;
				if (strstr(line, LEAD_KEY "="))
					saw_lead = 1;
			}
			fclose(f);
			CHECK(saw_pacer, "docs/ENV-KNOBS.md does not document %s", KEY);
			CHECK(saw_lead, "docs/ENV-KNOBS.md does not document %s", LEAD_KEY);
		}
	}

	if (fails) {
		printf("test_reac_pacer_knob: %d FAILED\n", fails);
		return 1;
	}
	printf("test_reac_pacer_knob: REACPW_PACER is per-segment, environment over "
	       "files, silent means thread, and both knobs are documented\n");
	return 0;
}
