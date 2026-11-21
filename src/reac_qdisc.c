// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The daemon's qdisc ownership. See reac_qdisc.h for why this is the daemon's job
 * and not an operator's `tc` line. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_qdisc.h"

#include <reac/transport/reac_etf_qdisc.h>

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the hardware form is held for the daemon's life ----------------------- *
 *
 * CHANGING IT RESETS THE NIC. On igc (i225/i226) adding or removing an offloaded etf
 * switches the launch queue's TSN mode, and the driver does that with an adapter reset:
 * the link renegotiates (2 s at 100 Mbit, measured on an i226 bench 2026-11-15), the box loses its
 * master, the segment drops, and a qdisc released with the segment resets the NIC again
 * on the way out and once more on the way back in. That is a loop, and it was the first
 * thing the bench showed. So the form is installed ONCE per device, kept across segment
 * drops (it holds only REAC_ETF_QDISC_PRIO traffic on the launch queue; the knock and
 * everything else ride the other queues untouched), adopted by the next serve without
 * a reinstall, and removed once, when the daemon exits. */
#define HW_MAX 32
static int hw_owned[HW_MAX];
static int hw_n;
static int hw_atexit;

static int hw_is_owned(int idx)
{
	for (int i = 0; i < hw_n; i++)
		if (hw_owned[i] == idx)
			return 1;
	return 0;
}

static void hw_forget(int idx)
{
	for (int i = 0; i < hw_n; i++)
		if (hw_owned[i] == idx) {
			hw_owned[i] = hw_owned[--hw_n];
			return;
		}
}

void reac_qdisc_release_all(void)
{
	for (int i = 0; i < hw_n; i++) {
		char name[IF_NAMESIZE] = "?";
		(void)if_indextoname((unsigned)hw_owned[i], name);
		if (reac_etf_qdisc_offloaded(hw_owned[i]) != 1)
			continue;   /* not ours any more, or the device is gone */
		int rc = reac_etf_qdisc_remove(hw_owned[i]);
		if (rc == 0)
			fprintf(stderr, "reac-qdisc: removed the hardware launch this daemon "
			        "installed on %s (exit)\n", name);
		else
			fprintf(stderr, "reac-qdisc: could not remove %s's hardware launch on "
			        "exit (errno %d) — %s. REMOVE IT BY HAND (`tc qdisc del dev %s "
			        "root`)\n", name, -rc, reac_etf_qdisc_fix(rc), name);
	}
	hw_n = 0;
}

static void hw_remember(int idx)
{
	if (hw_is_owned(idx) || hw_n >= HW_MAX)
		return;
	hw_owned[hw_n++] = idx;
	if (!hw_atexit) {
		hw_atexit = 1;
		atexit(reac_qdisc_release_all);
	}
}

const char *reac_qdisc_launch(int ifindex)
{
	if (ifindex <= 0)
		return "none";
	int off = reac_etf_qdisc_offloaded(ifindex);
	if (off < 0)
		return "unreadable";
	if (off == 1)
		return "hardware";
	return reac_etf_qdisc_state(ifindex, NULL, 0) == REAC_ETF_QDISC_PRESENT ? "software" : "none";
}

int reac_qdisc_arm(struct reac_qdisc *q, const char *ifname, int want_etf)
{
	if (!q)
		return -EINVAL;
	/* WHAT THIS DAEMON ALREADY OWNS HERE. Cleared with the rest, and put back in
	 * exactly one case below: a removal of OUR OWN etf that failed. */
	struct reac_qdisc ours = *q;
	memset(q, 0, sizeof *q);
	if (!ifname || !ifname[0])
		return -EINVAL;

	unsigned idx = if_nametoindex(ifname);
	if (!idx) {
		fprintf(stderr, "reac-qdisc: no such interface '%s' — the qdisc is not "
		        "touched\n", ifname);
		return -ENODEV;
	}
	strncpy(q->ifname, ifname, sizeof q->ifname - 1);

	char kind[32] = { 0 };
	enum reac_etf_qdisc_state before = reac_etf_qdisc_state((int)idx, kind, sizeof kind);

	if (!want_etf) {
		/* THE THREAD BACKEND UNDER A LEFTOVER ETF QDISC TRANSMITS NOTHING. Look
		 * first, then delete: an etf root we find is removed, and anything else —
		 * fq_codel, noqueue, somebody's pfifo_fast — is left alone, because it is
		 * not ours to take away. */
		if (before != REAC_ETF_QDISC_PRESENT) {
			if (before == REAC_ETF_QDISC_UNREADABLE)
				fprintf(stderr, "reac-qdisc: could not read %s's qdisc table; "
				        "if the thread backend transmits nothing, check "
				        "`tc qdisc show dev %s` for a leftover etf\n",
				        ifname, ifname);
			return 0;
		}
		int mine = ours.installed && ours.ifindex == (int)idx;
		int rc = reac_etf_qdisc_remove((int)idx);
		if (rc == 0)
			hw_forget((int)idx);
		if (rc != 0) {
			if (mine) {
				/* STILL THERE AND STILL OURS. The record goes back so the exit
				 * retries it; a cleared record here is a qdisc nobody ever takes
				 * away, dropping every unstamped frame on this device (#109). */
				*q = ours;
				fprintf(stderr, "reac-qdisc: the etf qdisc this daemon installed on "
				        "%s could not be taken back now (errno %d) — %s. It stays "
				        "on record and is tried again on exit; until it is gone, "
				        "every frame sent here without a launch time is dropped.\n",
				        ifname, -rc, reac_etf_qdisc_fix(rc));
				return rc;
			}
			fprintf(stderr, "reac-qdisc: %s carries a LEFTOVER etf qdisc and it "
			        "could not be removed (errno %d) — %s. On the thread backend "
			        "that qdisc drops every frame this daemon sends, because "
			        "skip_sock_check refuses a packet with no launch time.\n",
			        ifname, -rc, reac_etf_qdisc_fix(rc));
			return rc;
		}
		if (mine)
			fprintf(stderr, "reac-qdisc: removed the etf qdisc this daemon installed "
			        "on %s — the thread backend is running and that qdisc would have "
			        "dropped every frame it sends\n", ifname);
		else
			fprintf(stderr, "reac-qdisc: removed a LEFTOVER etf qdisc from %s — the "
			        "thread backend is running and that qdisc would have dropped "
			        "every frame it sends\n", ifname);
		return 0;
	}

	/* THE HARDWARE FORM FIRST, WHERE THE DEVICE CAN TAKE IT. Each condition is read
	 * (reac_qdisc.h); the first that fails is named once and the software form below
	 * is what this device gets, exactly as before. */
	if (hw_is_owned((int)idx) && reac_etf_qdisc_offloaded((int)idx) == 1) {
		/* OURS, STILL ON: adopted, not reinstalled — a reinstall resets the NIC. */
		q->ifindex   = (int)idx;
		q->installed = 1;
		q->offload   = 1;
		fprintf(stderr, "reac-qdisc: HARDWARE LAUNCH on %s kept from this daemon's "
		        "earlier serve (not reinstalled: changing it resets the NIC)\n", ifname);
		return 0;
	}
	char why[200];
	long long phc_off = 0;
	if (reac_qdisc_hw_capable(ifname, &phc_off, why, sizeof why)) {
		int hrc = reac_etf_qdisc_install_offload((int)idx, REAC_ETF_QDISC_DELTA_NS);
		if (hrc == 0 && reac_etf_qdisc_offloaded((int)idx) == 1) {
			q->ifindex   = (int)idx;
			q->installed = 1;
			q->offload   = 1;
			hw_remember((int)idx);
			fprintf(stderr, "reac-qdisc: HARDWARE LAUNCH on %s: mqprio root, etf "
			        "clockid CLOCK_TAI delta %u offload skip_sock_check on the launch "
			        "queue (%s); the NIC sends each frame at its launch time; held for "
			        "this daemon's life and removed when it exits\n", ifname,
			        REAC_ETF_QDISC_DELTA_NS, why);
			return 0;
		}
		if (hrc == 0) {
			/* ACKed, and the kernel does not report offload on: not claimed. */
			(void)reac_etf_qdisc_remove((int)idx);
			snprintf(why, sizeof why, "the offload was ACKed and the kernel does not "
			         "report it on");
		} else {
			snprintf(why, sizeof why, "the kernel refused `etf ... offload` here "
			         "(errno %d, %s)", -hrc, reac_etf_qdisc_fix(hrc));
		}
	}
	fprintf(stderr, "reac-qdisc: software ETF on %s, not the hardware launch: %s\n",
	        ifname, why);

	int rc = reac_etf_qdisc_install((int)idx, REAC_ETF_QDISC_DELTA_NS);
	if (rc != 0) {
		/* ONE LOUD LINE, NAMING THE ERRNO AND THE FIX. Not fatal: the pacer's own
		 * probe will find no etf qdisc and the ETF default will fall back to the
		 * thread backend, saying so. Two layers, one event — this is the half that
		 * can name the errno. */
		fprintf(stderr, "reac-qdisc: CANNOT INSTALL the etf qdisc on %s (errno %d) "
		        "— %s. The launch-time pacer needs it; without it this daemon runs "
		        "the thread backend.\n", ifname, -rc, reac_etf_qdisc_fix(rc));
		return rc;
	}

	/* PRESENCE-VERIFY THE WRITE. An ACKed add is good evidence, but the question
	 * the pacer is about to ask is "is there an etf qdisc on this device", and that
	 * is the question this answers. A write counted without a read-back is the
	 * blind-pass shape. */
	if (reac_etf_qdisc_state((int)idx, NULL, 0) != REAC_ETF_QDISC_PRESENT) {
		fprintf(stderr, "reac-qdisc: the kernel ACKed an etf qdisc on %s and the "
		        "read-back does not see one — not claiming it\n", ifname);
		return -EIO;
	}

	q->ifindex   = (int)idx;
	q->installed = 1;
	fprintf(stderr, "reac-qdisc: installed etf clockid CLOCK_TAI delta %u "
	        "skip_sock_check as %s's root qdisc (it carried '%s'); it is removed "
	        "again on exit\n", REAC_ETF_QDISC_DELTA_NS, ifname,
	        kind[0] ? kind : "nothing");
	return 0;
}

int reac_qdisc_disarm(struct reac_qdisc *q, const char *ifname, const char *refusal)
{
	if (!q)
		return -EINVAL;
	if (!refusal)
		refusal = "no reason given";
	if (q->installed)
		fprintf(stderr, "reac-qdisc: ETF was wanted on '%s' and the pacer refused it "
		        "(%s) — the etf qdisc this daemon just installed is REMOVED again, "
		        "because a thread-backend frame carries no launch time and "
		        "skip_sock_check would drop every one of them: the wire would carry "
		        "nothing and the journal would count pushes anyway\n",
		        ifname ? ifname : "?", refusal);
	else
		fprintf(stderr, "reac-qdisc: ETF was wanted on '%s' and the pacer refused it "
		        "(%s) — this daemon had installed NO etf qdisc there (the install "
		        "was refused above), so there is nothing of its own to take back; "
		        "the device is checked for a leftover etf from elsewhere, because "
		        "one would drop every thread-backend frame all the same\n",
		        ifname ? ifname : "?", refusal);
	return reac_qdisc_arm(q, ifname, 0);
}

void reac_qdisc_release(struct reac_qdisc *q)
{
	if (!q || !q->installed || q->ifindex <= 0) {
		if (q)
			memset(q, 0, sizeof *q);
		return;
	}

	if (q->offload && hw_is_owned(q->ifindex)) {
		/* HELD, NOT RELEASED: removing it resets the NIC (above). The exit takes it. */
		memset(q, 0, sizeof *q);
		return;
	}
	/* VERIFY, THEN DELETE. If something else has replaced our qdisc since — an
	 * operator, a second daemon — it is not ours to remove any more. */
	if (reac_etf_qdisc_state(q->ifindex, NULL, 0) == REAC_ETF_QDISC_PRESENT) {
		int rc = reac_etf_qdisc_remove(q->ifindex);
		if (rc == 0)
			fprintf(stderr, "reac-qdisc: removed the etf qdisc this daemon "
			        "installed on %s\n", q->ifname);
		else
			fprintf(stderr, "reac-qdisc: could not remove %s's etf qdisc on exit "
			        "(errno %d) — %s. REMOVE IT BY HAND (`tc qdisc del dev %s "
			        "root`): anything that transmits on this segment without a "
			        "launch time will be dropped.\n",
			        q->ifname, -rc, reac_etf_qdisc_fix(rc), q->ifname);
	}
	memset(q, 0, sizeof *q);
}

/* ---- the counters are libreac's ------------------------------------------- *
 *
 * reac_etf_qdisc_stats_read (<reac/transport/reac_etf_qdisc.h>) is the one RTM_GETQDISC
 * counter read; the health line calls it. This file used to carry a copy of that dump
 * walker, which ONE HOME (libreac's 2026-09-29-shared-code-has-one-home) retired in
 * 1.0.42: libreac 1.10.0 fuzzes the one reader, and tests/reac-code-conformance.py
 * refuses an RTM_GETQDISC request built here. */

int reac_qdisc_etf_catchup_slots(unsigned lead_us, int fps)
{
	if (fps <= 0)
		return 1;
	unsigned delta_us = REAC_ETF_QDISC_DELTA_NS / 1000u;
	unsigned usable = lead_us > delta_us ? lead_us - delta_us : 0u;
	/* A lead at or inside the qdisc's delta cannot absorb ANY lateness, so there is
	 * no repayable debt — but 0 means "the library's default" to the pacer's cfg,
	 * which is the opposite of what this says, so the floor is 1 slot. */
	unsigned long long n = ((unsigned long long)usable * (unsigned long long)fps) / 1000000ull;
	if (n < 1)
		return 1;
	if (n > 1000000ull)
		return 1000000;   /* a cfg field is an int; nothing sane reaches this */
	return (int)n;
}
