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
#include <string.h>

int reac_qdisc_arm(struct reac_qdisc *q, const char *ifname, int want_etf)
{
	if (!q)
		return -EINVAL;
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
		int rc = reac_etf_qdisc_remove((int)idx);
		if (rc != 0) {
			fprintf(stderr, "reac-qdisc: %s carries a LEFTOVER etf qdisc and it "
			        "could not be removed (errno %d) — %s. On the thread backend "
			        "that qdisc drops every frame this daemon sends, because "
			        "skip_sock_check refuses a packet with no launch time.\n",
			        ifname, -rc, reac_etf_qdisc_fix(rc));
			return rc;
		}
		fprintf(stderr, "reac-qdisc: removed a LEFTOVER etf qdisc from %s — the "
		        "thread backend is running and that qdisc would have dropped every "
		        "frame it sends\n", ifname);
		return 0;
	}

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

void reac_qdisc_release(struct reac_qdisc *q)
{
	if (!q || !q->installed || q->ifindex <= 0) {
		if (q)
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
