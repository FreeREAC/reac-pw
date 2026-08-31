// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE LADDER LAW, PINNED (issue #31).
 *
 * reac-pw ran its wire clocks at SCHED_FIFO 79 on a host whose PipeWire graph
 * driver runs at 60 and whose client data-loops run at 55. It preempted the
 * audio cycle it is fed by; the RME input accumulated ~1500 xruns and the
 * operator heard metallic artefacts on music. Nothing in the daemon reported a
 * fault, because from reac-pw's side nothing WAS faulty — the damage lands on
 * another device entirely.
 *
 * A number that can do that must not be defended by prose alone. The first
 * block asserts the header's tier constants against the priorities MEASURED on
 * the rig (`ps -eLo cls,rtprio,comm`, /usr/share/pipewire/pipewire.conf), so the
 * law is checked against the wire it describes rather than against itself: move
 * REAC_RT_PRIO_DEFAULT back over the graph, or quietly redefine a tier to make
 * it fit, and this goes red.
 *
 * The rest pins the knob: an operator can move the priority, a value that is not
 * a priority is REFUSED rather than coerced into one, and the layer that decided
 * is the layer reported. */

#include "reac_rt.h"
#include "reac_conf.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- measured on the rig, 2026-08-31 ------------------------------------- */
#define MEASURED_PW_DRIVER   60   /* pipewire data-loop.0 — the graph driver     */
#define MEASURED_PW_CLIENT   55   /* omx-mixer / mod-host / our own data-loop    */
#define MEASURED_KERNEL_IRQ  50   /* threaded IRQs, the REAC NICs among them     */
#define MEASURED_REGRESSION  79   /* what reac-pw ran at while the RME xrun'd    */

static char home[512];

static void wr(const char *rel, const char *body)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", home, rel);
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

static void rm_(const char *rel)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", home, rel);
	unlink(path);
}

#define SEG  ".config/reac-pw/eth0.env"
#define HOST ".config/reac-pw/reac-pw.env"

static void test_ladder(void)
{
	/* The tiers the header claims are the tiers the rig runs. */
	assert(REAC_RT_PW_DRIVER_PRIO  == MEASURED_PW_DRIVER);
	assert(REAC_RT_PW_CLIENT_PRIO  == MEASURED_PW_CLIENT);
	assert(REAC_RT_KERNEL_IRQ_PRIO == MEASURED_KERNEL_IRQ);

	/* Below EVERY thread the audio cycle needs — the driver, the clients that
	 * must answer within the same quantum, and the threaded IRQs that carry
	 * both the REAC frames and the audio interface. */
	assert(REAC_RT_PRIO_DEFAULT < MEASURED_KERNEL_IRQ);
	assert(REAC_RT_PRIO_DEFAULT < MEASURED_PW_CLIENT);
	assert(REAC_RT_PRIO_DEFAULT < MEASURED_PW_DRIVER);

	/* Still SCHED_FIFO, and therefore still ahead of every SCHED_OTHER task on
	 * the box: the fix is a demotion within the RT band, not out of it. */
	assert(REAC_RT_PRIO_DEFAULT >= 1);

	/* The regression itself, and the boundary either side of it. Equality with
	 * the client tier counts as contention: a runnable thread at the same
	 * priority still makes a graph loop wait for the CPU. */
	assert(reac_rt_prio_preempts_audio(MEASURED_REGRESSION));
	assert(reac_rt_prio_preempts_audio(MEASURED_PW_DRIVER));
	assert(reac_rt_prio_preempts_audio(MEASURED_PW_CLIENT));
	assert(!reac_rt_prio_preempts_audio(MEASURED_PW_CLIENT - 1));
	assert(!reac_rt_prio_preempts_audio(REAC_RT_PRIO_DEFAULT));
}

static void test_parse(void)
{
	enum reac_rt_prio_source src;

	/* Not an answer: unset, empty, blank. The built-in is in force and says so. */
	assert(reac_rt_prio_parse(NULL, &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_BUILTIN);
	assert(reac_rt_prio_parse("", &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_BUILTIN);
	assert(reac_rt_prio_parse("   ", &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_BUILTIN);

	/* A usable priority, surrounding blanks tolerated. */
	assert(reac_rt_prio_parse("40", &src) == 40 && src == REAC_RT_PRIO_SRC_CONFIG);
	assert(reac_rt_prio_parse("  7\n", &src) == 7 && src == REAC_RT_PRIO_SRC_CONFIG);
	assert(reac_rt_prio_parse("1", &src) == 1 && src == REAC_RT_PRIO_SRC_CONFIG);
	assert(reac_rt_prio_parse("99", &src) == 99 && src == REAC_RT_PRIO_SRC_CONFIG);

	/* An operator MAY put us over the graph — they may know something about
	 * their host that we do not. It is honoured, and reac_rt_thread_go says
	 * loudly what it costs. */
	assert(reac_rt_prio_parse("80", &src) == 80 && src == REAC_RT_PRIO_SRC_CONFIG);
	assert(reac_rt_prio_preempts_audio(80));

	/* Not a priority: REFUSED, and the built-in — never the bad number coerced
	 * into range, because a priority silently clamped to something nobody wrote
	 * is exactly the number that gets believed. */
	const char *junk[] = { "0", "100", "-1", "-40", "abc", "45x", "45 60", "4.5", "0x2d" };
	for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
		assert(reac_rt_prio_parse(junk[i], &src) == REAC_RT_PRIO_DEFAULT);
		assert(src == REAC_RT_PRIO_SRC_REFUSED);
	}

	/* Every source names itself: these strings go in front of an operator. */
	for (int s = REAC_RT_PRIO_SRC_BUILTIN; s <= REAC_RT_PRIO_SRC_REFUSED; s++) {
		const char *n = reac_rt_prio_source_name((enum reac_rt_prio_source)s);
		assert(n && *n);
	}
}

static void test_resolve(void)
{
	enum reac_rt_prio_source src;

	/* Nothing configured anywhere. */
	assert(reac_rt_prio_resolve(0, home, &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_BUILTIN);

	/* The per-host file answers — the layer an operator retunes a host in. */
	wr(HOST, "# this host's ladder\n" REAC_RT_PRIO_KEY "=40\n");
	assert(reac_rt_prio_resolve(0, home, &src) == 40);
	assert(src == REAC_RT_PRIO_SRC_CONFIG);

	/* The process environment outranks the file (reac_conf.h precedence). */
	setenv(REAC_RT_PRIO_KEY, "38", 1);
	assert(reac_rt_prio_resolve(0, home, &src) == 38);
	assert(src == REAC_RT_PRIO_SRC_CONFIG);

	/* An explicit caller value outranks both: a rig host that pins the pacer
	 * deliberately is not overridden by a file it did not write. */
	assert(reac_rt_prio_resolve(33, home, &src) == 33);
	assert(src == REAC_RT_PRIO_SRC_CALLER);
	unsetenv(REAC_RT_PRIO_KEY);

	/* A configured value that is not a priority does not take the daemon down
	 * and does not become the priority: the built-in runs and is reported as
	 * having taken over. */
	wr(HOST, REAC_RT_PRIO_KEY "=turbo\n");
	assert(reac_rt_prio_resolve(0, home, &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_REFUSED);
	rm_(HOST);

	/* The scheduler ladder is a property of the HOST, so the per-segment layer
	 * is deliberately not consulted — one NIC's file must not decide where the
	 * OTHER segment's wire clock sits, nor silently move a whole daemon. */
	wr(SEG, REAC_RT_PRIO_KEY "=52\n");
	assert(reac_rt_prio_resolve(0, home, &src) == REAC_RT_PRIO_DEFAULT);
	assert(src == REAC_RT_PRIO_SRC_BUILTIN);
	rm_(SEG);
}

int main(void)
{
	char tmpl[] = "/tmp/reac_rt_testXXXXXX";
	assert(mkdtemp(tmpl));
	snprintf(home, sizeof home, "%s", tmpl);
	unsetenv(REAC_RT_PRIO_KEY);

	test_ladder();
	test_parse();
	test_resolve();

	char cmd[1024];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not clean %s\n", tmpl);

	printf("test_reac_rt: OK\n");
	return 0;
}
