// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REACPW_PACER — the knob that chooses which backend owns the egress instant.
 *
 * WHAT THIS PINS, AND WHAT IT DELIBERATELY DOES NOT. The mapping from the word to
 * the backend, and the refusal when a precondition is missing, live in
 * libreac-transport and are tested there (`test_reac_etf`, `make test`). What is
 * reac-pw's own contract is the part an OPERATOR touches: the knob's NAME, the fact
 * that it rides the same layered precedence as every other knob — so a segment can
 * be put on one arm and its neighbour on the other for a comparison — and WHAT
 * SILENCE MEANS.
 *
 * SILENCE MEANS ETF since the operator's ruling of 2026-09-14 ("we must go with
 * qdisc and etf"), and that is asserted here through reac_pacer_backend_resolve
 * rather than through the raw lookup, because the default is not a value any layer
 * carries: it is what the resolver answers when every layer is quiet. A test that
 * only checked "no layer answered" was green on both sides of the ruling.
 *
 * That last one is the reason this file exists. The comparative run puts one
 * segment on `etf` and one on `thread`; if the per-segment layer did not work for
 * this key, both segments would quietly run the same backend and the table would
 * print two columns of the same measurement. Nothing else would say so.
 *
 * It is the layer lookup that is asserted, not a pacer: opening a pacer needs a NIC
 * and CAP_NET_RAW, and neither belongs in a unit test. */

#include <reac/transport/reac_conf.h>
#include <reac/transport/reac_pacer.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEY "REACPW_PACER"
#define LEAD_KEY "REACPW_PACER_LEAD_US"

static char home[512];
static int fails;

/* A SCRATCH HOME THAT IS THIS RUN'S ALONE, AND GONE WHEN IT ENDS. It used to be
 * /tmp/reacpw-knob-<pid>, created and never removed: a later run that drew the same pid
 * found the earlier run's ~/.config/reac-pw/reac-pw.env already there, and every "nothing
 * is set" arm read REACPW_PACER=thread from a file this run never wrote. mkdtemp makes
 * the directory unique, and cleanup() removes it on every way out: a return (atexit) and
 * a fatal signal (the handler, then the signal re-raised so the exit status still says
 * what happened). The test only ever writes one file (HOST_REL), so cleanup() unlinks
 * exactly that and the directories above it — unlink and rmdir, both async-signal-safe. */
#define HOST_REL ".config/reac-pw/reac-pw.env"
static char p_file[640], p_dir1[640], p_dir2[640];

static void cleanup(void)
{
	if (!home[0])
		return;
	unlink(p_file);
	rmdir(p_dir1);
	rmdir(p_dir2);
	rmdir(home);
}

static void on_fatal(int sig)
{
	cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

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
	if (!f) {
		fails++;
		printf("FAIL %s:%d: could not write %s\n", __FILE__, __LINE__, path);
		return;
	}
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(home, sizeof home, "%s/reacpw-knob-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(home)) {
		printf("FAIL %s:%d: mkdtemp(%s) failed — no scratch HOME, NOT A RESULT\n",
		       __FILE__, __LINE__, home);
		return 1;
	}
	snprintf(p_file, sizeof p_file, "%s/%s", home, HOST_REL);
	snprintf(p_dir1, sizeof p_dir1, "%s/.config/reac-pw", home);
	snprintf(p_dir2, sizeof p_dir2, "%s/.config", home);
	atexit(cleanup);
	signal(SIGABRT, on_fatal);
	signal(SIGSEGV, on_fatal);
	signal(SIGBUS, on_fatal);
	signal(SIGTERM, on_fatal);
	signal(SIGINT, on_fatal);

	char v[64];
	enum reac_conf_layer l;

	/* ---- 0. SILENCE IS ETF (operator ruling, 2026-09-14) -------------------- *
	 * The measurement behind the ruling, on the TX device: interval sd 28.5 -> 2.7
	 * us on the PCI VLAN and 15.3 -> 1.9 us on the USB link, late slots 27-37/s ->
	 * 0.45/s. This asserts the RESOLVER's answer, not the lookup's layer: the
	 * default is not a value any layer carries, so a test that only checked "no
	 * layer answered" is green whichever way the default points — and was.
	 *
	 * `layer` is the other half and it is load-bearing. REAC_CONF_NONE is how the
	 * pacer knows that NOBODY ASKED, which is what lets a missing precondition fall
	 * back to the thread backend instead of refusing to open. An explicit "etf"
	 * must never reach the pacer looking like a default. */
	{
		enum reac_conf_layer blay = (enum reac_conf_layer)-1;
		int understood = 0;
		unsetenv(KEY);
		enum reac_pacer_backend be =
			reac_pacer_backend_resolve("enp131s0.11", home, &blay, &understood);
		CHECK(be == REAC_PACER_BACKEND_ETF,
		      "with nothing set the backend resolved to '%s'; the default is etf",
		      reac_pacer_backend_name(be));
		CHECK(blay == REAC_CONF_NONE,
		      "an unset knob claimed layer %s — the pacer reads this to know that "
		      "nobody asked", reac_conf_layer_name(blay));
		CHECK(understood == 1, "an unset knob was reported as not understood");

		/* THE OPT-OUT STILL WORKS, and it arrives as a CHOICE (a layer answered),
		 * which is what makes an ETF refusal fatal for an explicit ask and
		 * survivable for the default. */
		setenv(KEY, "thread", 1);
		be = reac_pacer_backend_resolve("enp131s0.11", home, &blay, &understood);
		CHECK(be == REAC_PACER_BACKEND_THREAD,
		      "REACPW_PACER=thread resolved to '%s'", reac_pacer_backend_name(be));
		CHECK(blay != REAC_CONF_NONE, "an explicit 'thread' claimed no layer");

		setenv(KEY, "etf", 1);
		be = reac_pacer_backend_resolve("enp131s0.11", home, &blay, &understood);
		CHECK(be == REAC_PACER_BACKEND_ETF,
		      "REACPW_PACER=etf resolved to '%s'", reac_pacer_backend_name(be));
		CHECK(blay != REAC_CONF_NONE, "an explicit 'etf' claimed no layer — the "
		      "pacer would then treat a refusal as survivable");

		/* A word nobody can parse is not consent: the default is taken AND the
		 * caller is told, so the journal can say so. */
		setenv(KEY, "qdisc", 1);
		be = reac_pacer_backend_resolve("enp131s0.11", home, &blay, &understood);
		CHECK(understood == 0, "an unparseable backend name was accepted silently");
		CHECK(be == REAC_PACER_BACKEND_ETF,
		      "an unparseable backend name did not fall to the default");
		unsetenv(KEY);
	}

	/* ---- 1. AND NO LAYER ANSWERS WHEN NOTHING IS SET ------------------------ *
	 * The lookup's own half of the above. An EMPTY value is a key someone turned
	 * off, not a key set to "": it must fall through, never be taken as a backend
	 * name. */
	unsetenv(KEY);
	l = reac_conf_lookup(KEY, "enp131s0.11", home, v, sizeof v);
	CHECK(l == REAC_CONF_NONE, "an unset %s answered from layer %d (%s)", KEY,
	      (int)l, reac_conf_layer_name(l));

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
	wr(HOST_REL, "REACPW_PACER=thread\n");
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
	       "files, SILENT MEANS ETF, an explicit value always carries a layer, and "
	       "both knobs are documented\n");
	return 0;
}
