// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The ONE override file's grammar, and — the half that matters — what it REFUSES.
 * (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md §3a.)
 *
 * The rule is text, so it is pinned as text. Every refusal here is also asserted to NAME
 * its subject: a config reader that drops a line in silence is how an operator ends up
 * with a rig that is not doing what their file says, which is the exact defect the whole
 * spec exists to remove — one level up, it was a file the console wrote.
 *
 * tests/segments-autodetect.sh is the other half: every rule below can be right while
 * main.c never asks this module anything.
 *
 * No filesystem except one file this test writes, no netlink, no PipeWire. */
#include "reac_segconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* Does any kept refusal mention `needle`? The claim every refusal has to satisfy. */
static int refusal_names(const struct reac_segconf *c, const char *needle)
{
	for (int i = 0; i < c->n_refusals; i++)
		if (strstr(c->refusal[i], needle))
			return 1;
	return 0;
}

static enum reac_role_intent role_of(const struct reac_segconf *c, const char *name)
{
	enum reac_role_intent i;
	return reac_segconf_role(c, name, &i) ? i : REAC_ROLE_INTENT_AUTO;
}

int main(void)
{
	/* ---- 1. THE FILE AN OPERATOR ACTUALLY WRITES ------------------------------- */
	struct reac_segconf c;
	reac_segconf_init(&c);
	reac_segconf_parse(&c,
		"# ~/.config/reac-pw/reac-pw.conf\n"
		"\n"
		"[segment enp131s0.11]\n"
		"role = tap          ; a switch mirror\n"
		"\n"
		"[segment enp131s0.12]\n"
		"ignore = yes\n"
		"\n"
		"[segment enp131s0.1]\n"
		"role = \"master\"\n");
	CHECK(c.n == 3, "three segments named, got %d", c.n);
	CHECK(c.refused == 0, "a clean file refused %u line(s): %s", c.refused,
	      c.n_refusals ? c.refusal[0] : "");
	CHECK(role_of(&c, "enp131s0.11") == REAC_ROLE_INTENT_TAP, "role = tap did not read as tap");
	CHECK(role_of(&c, "enp131s0.1") == REAC_ROLE_INTENT_MASTER, "a QUOTED value did not unquote");
	CHECK(reac_segconf_ignored(&c, "enp131s0.12"), "ignore = yes did not ignore");
	CHECK(!reac_segconf_ignored(&c, "enp131s0.11"), "ignore leaked onto another segment");

	/* SILENCE IS THE DEFAULT, AND IT IS NOT A VALUE. A segment the file never mentions
	 * must answer "no pin" — not master, not auto-as-if-set. This is the whole ruling:
	 * with no file, the wire decides. */
	enum reac_role_intent got;
	CHECK(!reac_segconf_role(&c, "enp131s0.13", &got),
	      "a segment the file never names came back pinned");
	CHECK(!reac_segconf_ignored(&c, "enp131s0.13"),
	      "a segment the file never names came back ignored");
	CHECK(reac_segconf_find(&c, "ENP131S0.11") == NULL,
	      "segment names matched case-INsensitively; the kernel does not");

	/* An explicit `role = auto` is legal and says the same thing as silence — but it
	 * ANSWERED, so the daemon can report that the file said it. */
	reac_segconf_init(&c);
	reac_segconf_parse(&c, "[segment eth0]\nrole=auto\n");
	CHECK(reac_segconf_role(&c, "eth0", &got) && got == REAC_ROLE_INTENT_AUTO,
	      "an explicit role = auto did not read back");

	/* ---- 2. CASE, WHITESPACE AND COMMENT FORMS --------------------------------- */
	reac_segconf_init(&c);
	reac_segconf_parse(&c,
		"[SEGMENT eth0]\n"
		"   ROLE   =   TAP   \n"    /* keys and section keyword fold; values do too */
		"; a semicolon comment on its own line\n"
		"[Segment eth1]\n"
		"Ignore=TRUE\n");
	CHECK(role_of(&c, "eth0") == REAC_ROLE_INTENT_TAP, "the section keyword or key did not fold case");
	CHECK(reac_segconf_ignored(&c, "eth1"), "ignore = TRUE did not fold case");
	CHECK(c.refused == 0, "a case-varied file refused %u line(s): %s", c.refused,
	      c.n_refusals ? c.refusal[0] : "");

	/* A COMMENT MARKER DOES NOT EAT THE MIDDLE OF A TOKEN. `#` and `;` start a comment
	 * at the line start or after whitespace, and nowhere else. */
	reac_segconf_init(&c);
	reac_segconf_parse(&c, "[segment eth0]\nrole=tap#not-a-comment\n");
	CHECK(c.refused == 1 && refusal_names(&c, "tap#not-a-comment"),
	      "a '#' with no space before it was treated as a comment");

	/* ---- 3. EVERY REFUSAL NAMES ITS SUBJECT ------------------------------------ */
	reac_segconf_init(&c);
	reac_segconf_parse(&c,
		"[segment eth0]\n"
		"role = nonsense\n"        /* not in the vocabulary */
		"roel = tap\n"             /* a typo in the key */
		"ignore = perhaps\n"       /* not a boolean */
		"[daemon]\n"               /* not a section this file has */
		"[segment eth1\n"          /* unclosed */
		"role = tap\n"             /* ... so this one is outside any section */
		"[segment ]\n"             /* names nothing */
		"[segment averyveryverylongifname]\n"   /* longer than IFNAMSIZ-1 */
		"bare line with no equals\n");
	CHECK(refusal_names(&c, "nonsense"), "an unparsable role value was not named");
	CHECK(refusal_names(&c, "roel"), "an unknown key was not named");
	CHECK(refusal_names(&c, "perhaps"), "an unparsable ignore value was not named");
	CHECK(refusal_names(&c, "daemon"), "an unknown section was not named");
	CHECK(c.refused >= 7, "expected at least seven refusals, got %u", c.refused);
	/* AND THE REST OF THE FILE IS STILL HONOURED. A typo must be visible and must not
	 * take a desk down mid-show; the only way to have both is a refusal that is loud
	 * and local. eth0 survives its three bad lines. */
	CHECK(reac_segconf_find(&c, "eth0") != NULL, "one bad line threw away its whole section");
	CHECK(!reac_segconf_role(&c, "eth0", &got), "a refused role value was applied anyway");

	/* THE BOUND IS REPORTED, NOT HIT IN SILENCE. */
	reac_segconf_init(&c);
	char big[REAC_SEGCONF_MAX * 64 + 512];
	int off = 0;
	for (int i = 0; i < REAC_SEGCONF_MAX + 3; i++)
		off += snprintf(big + off, sizeof big - (size_t)off,
		                "[segment veth%d]\nrole=tap\n", i);
	reac_segconf_parse(&c, big);
	CHECK(c.n == REAC_SEGCONF_MAX, "the table holds %d, expected %d", c.n, REAC_SEGCONF_MAX);
	CHECK(c.overflow == 3, "three segments past the bound, counted %u", c.overflow);
	CHECK(refusal_names(&c, "bound"), "the bound was hit and not named");

	/* ---- 4. THE FILE ON DISK, PRESENT AND ABSENT ------------------------------- */
	char home[] = "/tmp/reac-segconf-XXXXXX";
	CHECK(mkdtemp(home) != NULL, "could not make a temp home");
	char dir[512], path[640], cmd[768];
	snprintf(dir, sizeof dir, "%s/%s", home, REAC_SEGCONF_DIR);
	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
	CHECK(system(cmd) == 0, "could not make the conf dir");
	snprintf(path, sizeof path, "%s/%s", dir, REAC_SEGCONF_FILE);

	/* ABSENT IS THE NORMAL CASE and must not read as an error or as an empty file that
	 * was there — `present` is what tells the two apart, and the daemon prints it. */
	reac_segconf_init(&c);
	CHECK(reac_segconf_load(&c, home) == 0, "an absent file named segments");
	CHECK(c.present == 0, "an absent file reported itself present");
	CHECK(c.refused == 0, "an absent file produced a refusal");
	CHECK(strcmp(c.path, path) == 0, "the path it looked at is '%s', expected '%s'", c.path, path);

	FILE *f = fopen(path, "we");
	CHECK(f != NULL, "could not write the conf");
	if (f) {
		fputs("[segment enp131s0.11]\nrole = tap\n", f);
		fclose(f);
	}
	reac_segconf_init(&c);
	CHECK(reac_segconf_load(&c, home) == 1, "the file on disk named no segment");
	CHECK(c.present == 1, "a file that was read reported itself absent");
	CHECK(role_of(&c, "enp131s0.11") == REAC_ROLE_INTENT_TAP, "the file on disk did not pin tap");

	/* PRESENT AND EMPTY IS NOT ABSENT. */
	f = fopen(path, "we");
	if (f) { fputs("# nothing here\n", f); fclose(f); }
	reac_segconf_init(&c);
	CHECK(reac_segconf_load(&c, home) == 0, "an empty file named a segment");
	CHECK(c.present == 1, "an empty file that EXISTS reported itself absent");

	unlink(path);
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
	if (system(cmd) != 0)
		printf("note: could not remove %s\n", home);

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
