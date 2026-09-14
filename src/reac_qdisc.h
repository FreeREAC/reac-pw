/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_qdisc — THE DAEMON OWNS THE QDISC ON THE DEVICE IT BINDS.
 *
 * WHOSE SETTING. An app owns its own configuration: reac-pw decides which pacing
 * backend runs on a segment, so reac-pw is what must make that segment's qdisc
 * match. Leaving it to an operator's `tc` line is the two-ledger shape — two doors
 * on one setting, neither announcing the other — and the rig has already paid for
 * it once from each side:
 *
 *   ETF BACKEND, NO QDISC. Every frame carries a SCM_TXTIME launch time and the
 *   kernel ignores every one of them. reac_repacer ran like that for months and it
 *   was found by an operator's ear.
 *
 *   THREAD BACKEND, LEFTOVER QDISC. With skip_sock_check the etf qdisc drops every
 *   frame that carries NO launch time, so the daemon transmits NOTHING. Measured
 *   2026-09-14 on the rig: 0 packets in a 60 s window and the box lost its master.
 *
 * So this is not "install a qdisc when we want one". It is: on every start, make the
 * device match the backend — install for etf, and REMOVE A LEFTOVER ETF for thread —
 * and on every clean exit, take away exactly what we put there.
 *
 * WE REMOVE ONLY WHAT WE INSTALLED, AND ONLY AFTER LOOKING. A delete is verified
 * before it is issued, never after: `armed` remembers the ifindex we actually added
 * a qdisc to, and the leftover sweep removes a root qdisc only when the netlink read
 * says it is an `etf`. A device carrying somebody else's fq_codel is never stripped
 * by us.
 *
 * The mechanism is libreac-transport's (<reac/transport/reac_etf_qdisc.h>): plain C
 * over rtnetlink, no tc(8), no subprocess. What lives here is the POLICY. */
#ifndef REAC_QDISC_H
#define REAC_QDISC_H

/* What the daemon did to one device's qdisc, so the exit can undo exactly that. Zero
 * initialised means "we touched nothing", which is what every non-master path is. */
struct reac_qdisc {
	int  ifindex;     /* the device we installed on; 0 = none */
	char ifname[16];  /* IFNAMSIZ, for the journal after the index may have gone */
	int  installed;   /* 1 once the add was ACKed by the kernel */
};

/* Make `ifname`'s qdisc match `want_etf`, and say what happened on stderr.
 *
 *   want_etf != 0  — install `etf clockid CLOCK_TAI delta 300000 skip_sock_check` as
 *                    the root qdisc (del-then-add; etf has no change operation).
 *   want_etf == 0  — remove an etf root if one is there, and say so. A device that
 *                    carries no etf is left exactly as it is.
 *
 * Returns 0 when the device now matches the backend, or -errno. A FAILURE IS NOT
 * FATAL AND IS NOT SILENT: the ETF default falls back to the thread backend when the
 * pacer's own probe then finds no qdisc, so the caller logs and continues — but the
 * errno and its fix are named here, once, loudly, because "cannot install" and "will
 * not run ETF" are the same event seen from two layers. */
int reac_qdisc_arm(struct reac_qdisc *q, const char *ifname, int want_etf);

/* Take away what reac_qdisc_arm installed, if anything, and clear `q`. Safe on a
 * zeroed struct and safe to call twice. A device that has gone since (the cable, the
 * VLAN un-minted) is not an error: there is no qdisc left to own. */
void reac_qdisc_release(struct reac_qdisc *q);

#endif /* REAC_QDISC_H */
