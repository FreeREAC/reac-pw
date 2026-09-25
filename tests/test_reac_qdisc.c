// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The two pieces of reac_qdisc that can be pinned without a wire.
 *
 * 1. THE ETF CATCH-UP BUDGET IS A DURATION, and the bug it replaces was a units
 *    bug of exactly the shape reac_pacer.h already warned about: a budget written
 *    for one reference (the thread's wake = the egress instant) left in force under
 *    a backend that moved the reference (the kernel's launch time, a lead ahead).
 *    These cases pin the arithmetic AND the two edges — a lead inside the qdisc's
 *    own delta cannot absorb anything, and 0 is not an answer this may return,
 *    because 0 means "the library default" to the pacer's cfg.
 *
 * 2. THE STATS READ SAYS UNREADABLE WHEN IT IS. A dump that completed and found no
 *    etf qdisc on the device answers 0 with qdiscs == 0; a call that could not ask
 *    answers -errno. Reporting 0 drops from a read that never happened is the
 *    absence-looks-like-silence failure this whole lane is about.
 *
 * 3. THE RECORD OF WHAT THIS DAEMON INSTALLED IS THE VERDICT, AND IT SURVIVES A
 *    REMOVAL THAT FAILED (#109). Two defects of one shape: the sink node discarded
 *    reac_qdisc_arm's answer and later announced "the etf qdisc this daemon just
 *    installed is REMOVED again" after an install that had been refused (no
 *    CAP_NET_ADMIN — nothing was ever installed); and reac_qdisc_arm's unconditional
 *    memset cleared `installed` even when taking OUR qdisc back failed, so the exit's
 *    reac_qdisc_release skipped a live etf that then dropped every unstamped frame.
 *    Neither can be provoked on a real device without privileges, so the three
 *    libreac-transport doors the policy calls are replaced at LINK time
 *    (-Wl,--wrap in meson.build): the policy under test is the real reac_qdisc.c,
 *    the kernel is a script. The wrap changes nothing about what the daemon links.
 *
 * No socket beyond one netlink dump of the host's own qdisc table, which is a read
 * and changes nothing; the arm/disarm cases touch no device at all. Safe beside a
 * live rig. */
#include "reac_qdisc.h"

#include <reac/transport/reac_etf_qdisc.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

/* ---- the scripted kernel (link seam) --------------------------------------- */
static enum reac_etf_qdisc_state fake_state = REAC_ETF_QDISC_NONE;
static int fake_install_rc, fake_remove_rc;
static int installs, removes;

enum reac_etf_qdisc_state __wrap_reac_etf_qdisc_state(int ifindex, char *kind, size_t cap)
{
	(void)ifindex;
	if (kind && cap)
		kind[0] = '\0';
	return fake_state;
}

int __wrap_reac_etf_qdisc_install(int ifindex, uint32_t delta_ns)
{
	(void)ifindex; (void)delta_ns;
	installs++;
	if (fake_install_rc == 0)
		fake_state = REAC_ETF_QDISC_PRESENT;
	return fake_install_rc;
}

int __wrap_reac_etf_qdisc_remove(int ifindex)
{
	(void)ifindex;
	removes++;
	if (fake_remove_rc == 0)
		fake_state = REAC_ETF_QDISC_NONE;
	return fake_remove_rc;
}

/* ---- stderr, captured: the LINE is part of the contract -------------------- */
static FILE *cap_file;
static int cap_saved = -1;

static void capture_begin(void)
{
	fflush(stderr);
	cap_file = tmpfile();
	cap_saved = dup(STDERR_FILENO);
	dup2(fileno(cap_file), STDERR_FILENO);
}

/* Returns what stderr said since capture_begin (malloc'd), and restores it. */
static char *capture_end(void)
{
	fflush(stderr);
	dup2(cap_saved, STDERR_FILENO);
	close(cap_saved);
	long n = ftell(cap_file);
	rewind(cap_file);
	char *buf = calloc(1, (size_t)n + 1);
	if (n > 0 && fread(buf, 1, (size_t)n, cap_file) != (size_t)n)
		buf[0] = '\0';
	fclose(cap_file);
	cap_file = NULL;
	return buf;
}

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

int main(void)
{
	/* The shipped default: 2500 us lead, 300 us qdisc delta -> 2200 us repayable. */
#define LEAD_SLOTS(fps) ((int)((2500u - REAC_ETF_QDISC_DELTA_NS / 1000u) * (unsigned long long)(fps) / 1000000u))
	CHECK(reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_96K) == LEAD_SLOTS(REAC_PKT_RATE_96K),
	      "2500 us lead at %d fps is %d slots, got %d", REAC_PKT_RATE_96K, LEAD_SLOTS(REAC_PKT_RATE_96K),
	      reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_96K));
	CHECK(reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_48K) == LEAD_SLOTS(REAC_PKT_RATE_48K),
	      "2500 us lead at %d fps is %d slots, got %d", REAC_PKT_RATE_48K, LEAD_SLOTS(REAC_PKT_RATE_48K),
	      reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_48K));
	CHECK(reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_44K1) == LEAD_SLOTS(REAC_PKT_RATE_44K1),
	      "2500 us lead at %d fps is %d slots, got %d", REAC_PKT_RATE_44K1, LEAD_SLOTS(REAC_PKT_RATE_44K1),
	      reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_44K1));

	/* THE BUDGET IT REPLACES. libreac's rate-derived default is 1000 us — 8 slots at
	 * 8000 fps — and the whole finding is that it is SMALLER than what the lead can
	 * absorb, so a wake the lead covers still re-based the launch grid. If this ever
	 * stops being true the fix has stopped being a fix. */
	CHECK(reac_qdisc_etf_catchup_slots(2500, REAC_PKT_RATE_96K) > 1000 * REAC_PKT_RATE_96K / 1000000,
	      "the ETF budget must exceed the thread backend's 1000 us default, "
	      "or nothing changed");

	/* A lead at or inside the qdisc's delta absorbs nothing — but 0 means "use the
	 * library default" to the cfg field this feeds, so the floor is one slot. */
	CHECK(reac_qdisc_etf_catchup_slots(300, REAC_PKT_RATE_96K) == 1,
	      "a lead equal to the qdisc delta floors at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(300, REAC_PKT_RATE_96K));
	CHECK(reac_qdisc_etf_catchup_slots(50, REAC_PKT_RATE_96K) == 1,
	      "a lead inside the qdisc delta floors at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(50, REAC_PKT_RATE_96K));
	CHECK(reac_qdisc_etf_catchup_slots(2500, 0) == 1,
	      "a zero rate cannot divide: floor at 1 slot, got %d",
	      reac_qdisc_etf_catchup_slots(2500, 0));

	/* Refusals by argument, not by guess. */
	struct reac_qdisc_stats st;
	CHECK(reac_qdisc_stats_read(0, &st) == -EINVAL, "ifindex 0 is refused");
	CHECK(reac_qdisc_stats_read(1, NULL) == -EINVAL, "a NULL out is refused");

	/* A DUMP THAT COMPLETES AND FINDS NOTHING IS NOT AN ERROR, and it is not a
	 * silent zero either: `qdiscs` says the sum covers nothing. ifindex 0x7ffffffe
	 * exists on no machine, so this is the "device carries no etf" answer. */
	memset(&st, 0xAA, sizeof st);
	int rc = reac_qdisc_stats_read(0x7ffffffe, &st);
	if (rc == 0) {
		CHECK(st.qdiscs == 0, "a device with no etf qdisc reports qdiscs=0, got %u",
		      st.qdiscs);
		CHECK(st.drops == 0, "and no drops, got %llu", st.drops);
	} else {
		/* No rtnetlink here (a sandbox with no NETLINK_ROUTE). Say so — a skip
		 * that prints nothing is the absence that looks like a pass. */
		printf("note: no rtnetlink in this environment (errno %d); the dump arm "
		       "did not run\n", -rc);
	}

	/* ---- 3. the record, under a scripted kernel. "lo" exists in every namespace. */
	struct reac_qdisc q;
	char *said;

	/* The control: an install the kernel ACKs and the read-back confirms IS recorded.
	 * Without this every "not installed" below could be a seam that never fired. */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_NONE; fake_install_rc = 0; fake_remove_rc = 0;
	installs = removes = 0;
	capture_begin();
	rc = reac_qdisc_arm(&q, "lo", 1);
	said = capture_end();
	CHECK(rc == 0 && installs == 1, "a scripted install is seen by the seam (rc %d, installs %d)", rc, installs);
	CHECK(q.installed == 1 && q.ifindex > 0, "an ACKed, read-back install is recorded");
	CHECK(strstr(said, "installed etf") != NULL, "and said");
	free(said);

	/* A REMOVAL OF OUR OWN QDISC THAT FAILS KEEPS THE RECORD (the leak): the qdisc is
	 * still on the device and still ours, so the exit must try again — and does. */
	fake_remove_rc = -EPERM;
	capture_begin();
	rc = reac_qdisc_arm(&q, "lo", 0);
	said = capture_end();
	CHECK(rc == -EPERM && removes == 1, "the removal ran and answered -EPERM (rc %d, removes %d)", rc, removes);
	CHECK(q.installed == 1 && q.ifindex > 0,
	      "the record of OUR install survives a removal that failed (installed=%d ifindex=%d)",
	      q.installed, q.ifindex);
	CHECK(strstr(said, "tried again on exit") != NULL, "and the line says the exit retries it");
	free(said);
	fake_remove_rc = 0;
	capture_begin();
	reac_qdisc_release(&q);
	said = capture_end();
	CHECK(removes == 2, "release RETRIED the removal (removes %d)", removes);
	CHECK(fake_state == REAC_ETF_QDISC_NONE && q.installed == 0, "and the device is clean");
	free(said);

	/* A REMOVAL OF OUR OWN QDISC THAT SUCCEEDS clears it, said as ours, not a leftover. */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_NONE; installs = removes = 0;
	capture_begin();
	(void)reac_qdisc_arm(&q, "lo", 1);
	rc = reac_qdisc_arm(&q, "lo", 0);
	said = capture_end();
	CHECK(rc == 0 && removes == 1 && q.installed == 0, "our qdisc taken back clears the record");
	CHECK(strstr(said, "removed the etf qdisc this daemon installed") != NULL, "and is named as ours");
	CHECK(strstr(said, "LEFTOVER") == NULL, "never as a leftover");
	free(said);

	/* NEVER ADOPT WHAT WE DID NOT INSTALL: a leftover that cannot be removed is reported
	 * and stays somebody else's — the record stays empty and the exit does not touch it. */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_PRESENT; fake_remove_rc = -EPERM; removes = 0;
	capture_begin();
	rc = reac_qdisc_arm(&q, "lo", 0);
	said = capture_end();
	CHECK(rc == -EPERM && q.installed == 0 && q.ifindex == 0,
	      "a leftover that cannot be removed is never recorded as ours");
	CHECK(strstr(said, "LEFTOVER") != NULL, "and is named a leftover");
	free(said);
	capture_begin();
	reac_qdisc_release(&q);
	said = capture_end();
	CHECK(removes == 1, "release does not touch what is not ours (removes %d)", removes);
	free(said);

	/* THE FALLBACK SAYS WHAT IS TRUE OF THIS DAEMON. Install refused (no CAP_NET_ADMIN),
	 * the pacer then refuses ETF: the line must NOT claim a qdisc was "just installed",
	 * and nothing is removed, because nothing of ours is there. */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_NONE; fake_install_rc = -EPERM; fake_remove_rc = 0;
	installs = removes = 0;
	capture_begin();
	rc = reac_qdisc_arm(&q, "lo", 1);
	said = capture_end();
	CHECK(rc == -EPERM && installs == 1 && q.installed == 0, "the refused install is not recorded");
	free(said);
	capture_begin();
	rc = reac_qdisc_disarm(&q, "lo", "TAI offset is 0");
	said = capture_end();
	CHECK(rc == 0 && removes == 0, "nothing of ours to remove, nothing removed (rc %d, removes %d)", rc, removes);
	CHECK(strstr(said, "just installed is REMOVED again") == NULL,
	      "a refused install is never announced as 'just installed is REMOVED again'");
	CHECK(strstr(said, "installed NO etf qdisc") != NULL,
	      "the line says this daemon installed nothing: got \"%s\"", said);
	CHECK(strstr(said, "TAI offset is 0") != NULL, "and quotes the pacer's refusal");
	free(said);

	/* And when it WAS installed, the fallback says so and takes it back. */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_NONE; fake_install_rc = 0; installs = removes = 0;
	capture_begin();
	(void)reac_qdisc_arm(&q, "lo", 1);
	rc = reac_qdisc_disarm(&q, "lo", "TAI offset is 0");
	said = capture_end();
	CHECK(rc == 0 && removes == 1 && q.installed == 0, "an installed qdisc is taken back by the fallback");
	CHECK(strstr(said, "just installed is REMOVED again") != NULL, "and announced as just installed");
	free(said);

	/* And a fallback whose removal fails keeps the record for the exit, like arm(0). */
	memset(&q, 0, sizeof q);
	fake_state = REAC_ETF_QDISC_NONE; installs = removes = 0;
	capture_begin();
	(void)reac_qdisc_arm(&q, "lo", 1);
	fake_remove_rc = -EPERM;
	rc = reac_qdisc_disarm(&q, "lo", NULL);
	said = capture_end();
	CHECK(rc == -EPERM && q.installed == 1, "a fallback whose removal failed keeps the record");
	free(said);
	fake_remove_rc = 0;
	capture_begin();
	reac_qdisc_release(&q);
	said = capture_end();
	CHECK(removes == 2 && q.installed == 0, "and the exit takes it back");
	free(said);

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
