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

/* ---- what the qdisc DID, not what it is ----------------------------------- *
 *
 * THE ONE NUMBER THAT SAYS A FRAME DID NOT LEAVE. Under ETF the pacer thread only
 * has to be EARLY: it hands the kernel a launch time a lead ahead (2500 us by
 * default) and the qdisc releases the frame at that instant. So the thread being
 * late is not a transmission fault — the LAUNCH TIME BEING IN THE PAST when the
 * frame reaches the qdisc is, and sch_etf answers that by dropping the packet and
 * counting it. Measured in tests/etf-late-is-the-wake.sh, one namespace, one load:
 *
 *   lead 2500 us   qdisc drops 11 in 25 s (0.44/s)   wire sd 3.2 us   8000.0 pps
 *   lead  400 us   qdisc drops 153 in 21 s (7.3/s)   wire sd 13.5 us  7992.9 pps
 *
 * and the pacer's own `late_wakes` read 12-46/s in BOTH — it does not move when the
 * wire breaks, and this does. That is why the health line reports this figure for
 * the ETF backend and why the wake lateness is reported beside it as a separate,
 * budgeted number rather than as "late".
 *
 * WHY THE READ IS HERE AND NOT IN THE LIBRARY. libreac-transport's public door
 * (reac_etf_qdisc_state) answers WHICH qdisc is on a device — the question the
 * pacer asks at open. The counters are a different question, asked every health
 * window by the daemon that owns the qdisc, so the daemon reads them. Same
 * rtnetlink socket shape, one RTM_GETQDISC dump, no tc(8) and no subprocess.
 *
 * UNREADABLE IS NOT ZERO. A dump that could not be made returns -errno and leaves
 * `out` untouched; a health line then says so rather than printing a 0 that would
 * read as "nothing was dropped". */
struct reac_qdisc_stats {
	unsigned long long packets;     /* summed over every etf qdisc on the device */
	unsigned long long bytes;
	unsigned long long drops;       /* THE figure: frames sch_etf would not launch */
	unsigned long long overlimits;
	unsigned int       qdiscs;      /* how many etf qdiscs the sum covers (0 = none) */
};

/* Read `ifindex`'s etf qdisc counters. Returns 0 on a dump that completed (with
 * `qdiscs` 0 when the device carries no etf), or -errno. */
int reac_qdisc_stats_read(int ifindex, struct reac_qdisc_stats *out);

/* ---- the catch-up budget under ETF ---------------------------------------- *
 *
 * The pacer repays overslept slots by staying on its grid, up to a budget, and
 * re-bases past it. libreac's default budget is 1000 us of MEASURED WAKE TAIL —
 * the right number for the thread backend, where an overslept slot is an egress
 * instant already lost.
 *
 * ETF MOVED THE REFERENCE THAT BUDGET IS MEASURED AGAINST. The thread now sleeps to
 * `launch - lead` and everything up to `lead - delta` of lateness still hands the
 * qdisc a launch time in the future: nothing is lost, and the debt is repayable by
 * definition. With the shipped defaults that is 2500 - 300 = 2200 us, more than
 * twice the budget in force — so a wake 1250 us late (worst debt 10 slots at 8000
 * fps, seen in the namespace run) re-based a launch grid that had 20 slots of lead
 * still in hand, and booked every one of those slots as abandoned. The budget was
 * right and its yardstick was stale.
 *
 * So under ETF the budget IS the lead the operator is running, minus the qdisc's
 * own delta, in slots. 2500 us at 8000 fps -> 17 slots; at 4000 fps -> 8. Rounds
 * down, and never below 1. An operator who SET REACPW_CATCHUP_MAX_SLOTS keeps
 * exactly what they set (and -1 still means "never repay"); this resolves only the
 * 0 that means "the default". */
int reac_qdisc_etf_catchup_slots(unsigned lead_us, int fps);

#endif /* REAC_QDISC_H */
