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

	/* ---- 5. THE DROP-IN DIRECTORY (amendment 2026-09-16, third, §A) ------------
	 *
	 * The console needs a door that is not the operator's file, and the operator needs
	 * to keep the last word inside the same grammar. The order is the one every drop-in
	 * directory on this host already uses: the base file first, then `*.conf` in BYTE
	 * order, LAST WINS PER KEY. Everything below is that sentence, split into the parts
	 * that can break separately. */
	char dird[640];
	snprintf(dird, sizeof dird, "%s/%s", dir, REAC_SEGCONF_DIRD);
	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dird);
	CHECK(system(cmd) == 0, "could not make the conf.d dir");

	/* The base file, and two drop-ins that disagree with it and with each other. */
	f = fopen(path, "we");
	if (f) { fputs("[segment s0]\nrole = master\n[segment s2]\nignore = yes\n", f); fclose(f); }
	char dp[768];
	snprintf(dp, sizeof dp, "%s/20-local.conf", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment s0]\nrole = slave\n[segment s1]\nignore = yes\n", f); fclose(f); }
	snprintf(dp, sizeof dp, "%s/50-openmixer.conf", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment s0]\nrole = tap\n[segment s2]\nrole = master\n", f); fclose(f); }
	/* NOT `*.conf`: neither is read, and neither is a refusal either. */
	snprintf(dp, sizeof dp, "%s/notes.txt", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment sX]\nrole = tap\n", f); fclose(f); }
	snprintf(dp, sizeof dp, "%s/50-openmixer.conf.bak", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment sY]\nrole = tap\n", f); fclose(f); }

	reac_segconf_init(&c);
	reac_segconf_load(&c, home);
	CHECK(c.refused == 0, "a clean base + drop-in set refused %u line(s): %s", c.refused,
	      c.n_refusals ? c.refusal[0] : "");
	CHECK(role_of(&c, "s0") == REAC_ROLE_INTENT_TAP,
	      "the LAST drop-in did not win s0's role (a drop-in overrides the hand-written file)");
	CHECK(reac_segconf_ignored(&c, "s1"), "a drop-in's own segment was not read");
	/* A KEY NOBODY LATER MENTIONED KEEPS ITS EARLIER ANSWER. Last-wins is per KEY, not
	 * per section: the drop-in said `role` for s2 and said nothing about `ignore`. */
	CHECK(reac_segconf_ignored(&c, "s2"), "a later file's `role` erased an earlier `ignore`");
	CHECK(role_of(&c, "s2") == REAC_ROLE_INTENT_MASTER, "the drop-in's s2 role was not read");
	CHECK(reac_segconf_find(&c, "sX") == NULL, "a file that is not *.conf was read");
	CHECK(reac_segconf_find(&c, "sY") == NULL, "`.conf.bak` was read as a drop-in");

	/* WHICH FILE ANSWERED — the provenance §C requires, and what makes last-wins
	 * debuggable rather than merely defined. */
	const char *src = reac_segconf_role_file(&c, "s0");
	CHECK(src && strstr(src, "50-openmixer.conf"),
	      "s0's role names '%s' as its source, expected the drop-in that set it",
	      src ? src : "(none)");
	src = reac_segconf_role_file(&c, "s2");
	CHECK(src && strstr(src, "50-openmixer.conf"), "s2's role provenance is '%s'",
	      src ? src : "(none)");
	CHECK(reac_segconf_role_file(&c, "s9") == NULL,
	      "a segment nobody pinned reported a source file");
	/* THE FILES READ, IN READ ORDER, so the start block can print them. */
	CHECK(c.n_files == 3, "read %d file(s), expected the base plus two drop-ins", c.n_files);
	if (c.n_files == 3) {
		CHECK(strcmp(c.file[0], REAC_SEGCONF_FILE) == 0,
		      "the hand-written file was not read FIRST (got '%s')", c.file[0]);
		CHECK(strstr(c.file[1], "20-local.conf") != NULL,
		      "the drop-ins are not in byte order: [1] is '%s'", c.file[1]);
		CHECK(strstr(c.file[2], "50-openmixer.conf") != NULL,
		      "the drop-ins are not in byte order: [2] is '%s'", c.file[2]);
	}

	/* A REPEATED SECTION ACROSS FILES IS AN OVERRIDE (silent, above). WITHIN one file it
	 * is still a typo, and the refusal names the FILE as well as the line — with N files
	 * a bare line number points at nothing. */
	snprintf(dp, sizeof dp, "%s/90-dup.conf", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment s5]\nrole = tap\n[segment s5]\nrole = slave\n", f); fclose(f); }
	reac_segconf_init(&c);
	reac_segconf_load(&c, home);
	CHECK(refusal_names(&c, "90-dup.conf"), "a refusal did not name the file it came from");
	CHECK(refusal_names(&c, "90-dup.conf:3:"), "a refusal did not name its line");

	/* AN UNREADABLE DROP-IN IS REFUSED BY NAME AND IS NOT FATAL. A dangling symlink,
	 * because this test also runs as root in a container, where mode 000 is readable. */
	snprintf(cmd, sizeof cmd, "ln -sf /nonexistent-reac-pw '%s/40-broken.conf'", dird);
	CHECK(system(cmd) == 0, "could not make the dangling drop-in");
	reac_segconf_init(&c);
	reac_segconf_load(&c, home);
	CHECK(refusal_names(&c, "40-broken.conf"), "an unreadable drop-in was dropped in silence");
	CHECK(role_of(&c, "s0") == REAC_ROLE_INTENT_TAP,
	      "one unreadable drop-in threw away the files that were readable");

	/* THE RE-READ SEES A NEW DROP-IN. Same one-stat rule as the conf: the directory's
	 * own mtime moves when a file is added, which is why the walk costs two stats in the
	 * common case. A test that writes twice in one second is exactly what a
	 * seconds-resolution stamp cannot see, so this is also the guard for that. */
	CHECK(reac_segconf_refresh(&c) == 0, "an unchanged conf.d reloaded anyway");
	snprintf(dp, sizeof dp, "%s/95-late.conf", dird);
	f = fopen(dp, "we");
	if (f) { fputs("[segment s0]\nrole = slave\n", f); fclose(f); }
	CHECK(reac_segconf_refresh(&c) == 1, "a drop-in added under a running daemon was not seen");
	CHECK(role_of(&c, "s0") == REAC_ROLE_INTENT_SLAVE,
	      "the new last drop-in did not win s0's role after the re-read");
	/* AND A REWRITE OF ONE DROP-IN, IN THE SAME SECOND, IS ALSO SEEN. */
	f = fopen(dp, "we");
	if (f) { fputs("[segment s0]\nrole = master\n", f); fclose(f); }
	CHECK(reac_segconf_refresh(&c) == 1, "a drop-in rewritten in the same second was not seen");
	CHECK(role_of(&c, "s0") == REAC_ROLE_INTENT_MASTER, "the rewritten drop-in was not applied");

	/* ---- THE BOX ROLE AND ITS MODEL ROW (2026-09-17 spec §4) ----
	 * A box declares a MODEL, and a box that declares the wrong width to a mixer
	 * is a patch that silently lands on the wrong channels. So there is no
	 * default model, an unknown token is refused BY NAME with the table's own
	 * tokens listed, and `model` on any other role is refused too — a key read
	 * on one role and ignored on another is a trap with no upside. */
	{
		struct reac_segconf b;
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nrole = box\nmodel = s1608\n");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_BOX, "role = box did not parse");
		CHECK(reac_segconf_model(&b, "e0") &&
		      strcmp(reac_segconf_model(&b, "e0"), "s1608") == 0,
		      "model = s1608 was not carried");
		CHECK(b.refused == 0, "a well-formed box section was refused");

		/* THE VALUE FOLDS, the segment name does not — the same rule role has. */
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nROLE = BOX\nMODEL = FR4000\n");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_BOX, "ROLE = BOX did not fold");
		CHECK(reac_segconf_model(&b, "e0") &&
		      strcmp(reac_segconf_model(&b, "e0"), "fr4000") == 0,
		      "MODEL = FR4000 did not fold to the table's token");

		/* A ROW NOBODY HAS SEEN IS STILL A ROW: the operator's experiment. */
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nrole = box\nmodel = fr0040\n");
		CHECK(reac_segconf_model(&b, "e0") &&
		      strcmp(reac_segconf_model(&b, "e0"), "fr0040") == 0,
		      "the 40-channel experiment row was not accepted");

		/* box WITHOUT a model: refused by name, and the segment falls back to
		 * auto rather than presenting an arbitrary width to a mixer. */
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nrole = box\n");
		CHECK(b.refused == 1, "role = box with no model was not refused");
		CHECK(refusal_names(&b, "model"), "the refusal does not name the missing key");
		CHECK(refusal_names(&b, "e0"), "the refusal does not name the segment");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_AUTO,
		      "a box with no model did not fall back to auto");

		/* An unknown token names itself AND the tokens that exist. */
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nrole = box\nmodel = s9999\n");
		CHECK(b.refused >= 1, "an unknown model token was not refused");
		CHECK(refusal_names(&b, "s9999"), "the refusal does not name the token");
		CHECK(refusal_names(&b, "s1608"), "the refusal does not list the table's tokens");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_AUTO,
		      "a box whose model was refused did not fall back to auto");
		CHECK(reac_segconf_model(&b, "e0") == NULL,
		      "a refused model was kept anyway");

		/* `model` on any other role is refused, and that role SURVIVES it. */
		reac_segconf_init(&b);
		reac_segconf_parse(&b, "[segment e0]\nrole = tap\nmodel = s1608\n");
		CHECK(b.refused == 1, "model under role = tap was not refused");
		CHECK(refusal_names(&b, "model"), "the refusal does not name the key");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_TAP, "the tap pin was lost");
		CHECK(reac_segconf_model(&b, "e0") == NULL, "a model was kept on a tap");

		/* LAST WINS PER KEY, across files, for `model` exactly as for `role`. */
		reac_segconf_init(&b);
		reac_segconf_parse_file(&b, "[segment e0]\nrole = box\nmodel = s1608\n",
		                        "reac-pw.conf");
		reac_segconf_parse_file(&b, "[segment e0]\nmodel = s4000s\n",
		                        "reac-pw.conf.d/50-openmixer.conf");
		CHECK(reac_segconf_model(&b, "e0") &&
		      strcmp(reac_segconf_model(&b, "e0"), "s4000s") == 0,
		      "a later file did not override the model");
		CHECK(role_of(&b, "e0") == REAC_ROLE_INTENT_BOX,
		      "the role a later file was silent about did not survive");
	}

	unlink(path);
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
	if (system(cmd) != 0)
		printf("note: could not remove %s\n", home);

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
