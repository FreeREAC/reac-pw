/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_roster — EVERY segment this daemon runs, as one node's properties, and the DELTA
 * that keeps it there without churning the node.
 * (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md, amendment
 * 2026-09-16 third, §B.)
 *
 * THE HOLE THIS FILLS, NAMED IN THE SPEC THAT MADE IT. The second amendment took the
 * zero-port node off the graph — "a segment with NO recognised box must not appear in the
 * PipeWire graph at all" — and said, in its own words, what that cost: the console's
 * `/reac/segment` roster is a GRAPH SCAN, so a segment with no node has no row at all, not
 * even one that says `probing`. The operator's answer the same day was that not
 * autodetecting is an ERROR, which a console can only report about a segment it can SEE.
 * So the daemon publishes ONE node, `reac-pw`, with no ports and a media.class no session
 * manager knows, and every segment lives in its props. The per-box `reac.segment` props on
 * reac-capture / reac-playback are untouched: this is not a second door to a segment, it is
 * the daemon's own row.
 *
 * DERIVED EVERY TICK, STORED NOWHERE. Nothing writes a roster field. The caller rebuilds
 * the whole wanted roster from the tables that already hold the truth — the conf for
 * `ignored`, the listener's own engine for tap/slave/refused, the master's recognized box
 * for `established` — and this module answers ONLY the question "what changed?". A second
 * ledger of segment state would be a second answer to a question that already has one.
 *
 * AND THE ANSWER TO "WHAT CHANGED" IS USUALLY NOTHING, which is the point. A roster that
 * republishes itself every tick is a property-changed storm on every client, with the same
 * node id — the defect one door down from the node churn this node exists to avoid.
 *
 * PURE: no PipeWire, no netlink, no clock, no allocation. The node lives in
 * reac_roster_node.c; this half is provable offline (tests/test_reac_roster.c). */
#ifndef REAC_ROSTER_H
#define REAC_ROSTER_H

#include <net/if.h>   /* IFNAMSIZ */

/* The same bound as reac_segconf's table and reac_ifscan's: two trunks' worth of VLANs. */
#define REAC_ROSTER_MAX 32
/* name, state, model, role, source, width — see the key grammar below. */
#define REAC_ROSTER_FIELDS 6
/* The count key plus every field of every group. The worst case is exact: a group is
 * either in the new roster (fields) or leaving it (removals), never both. */
#define REAC_ROSTER_KV_MAX (1 + REAC_ROSTER_MAX * REAC_ROSTER_FIELDS)

/* WHAT A SEGMENT IS DOING, in the spec's words and no others. `probing` is the state the
 * whole amendment is about: a segment the daemon is sniffing that has decided nothing —
 * no box, no node, and until now no row anywhere but the journal. */
enum reac_roster_state {
	REAC_ROSTER_PROBING = 0,
	REAC_ROSTER_ESTABLISHED,
	REAC_ROSTER_SLAVE,
	REAC_ROSTER_TAP,
	REAC_ROSTER_REFUSED,
	REAC_ROSTER_IGNORED,
};

const char *reac_roster_state_name(enum reac_roster_state s);

struct reac_roster_seg {
	char name[IFNAMSIZ];              /* the interface name IS the segment's identity */
	enum reac_roster_state state;
	char model[32];                   /* the recognised box, or `none` */
	char role[16];                    /* auto|master|slave|tap, as RESOLVED */
	char source[112];                 /* `autodetected` or `conf:<file>` */
	int in, out;                      /* published as `in/out`; 0/0 when there is no pair */
};

/* One property to set, or to REMOVE. `remove` is not "set it to empty": a group that left
 * must leave no key behind, because an empty string and an absent key must not read alike
 * — the same law reac_segconf's `present` flag is built on. */
struct reac_roster_kv {
	char key[40];
	char val[112];
	int  remove;
};

struct reac_roster {
	struct reac_roster_seg pub[REAC_ROSTER_MAX];   /* what the node currently carries */
	int n_pub;
	struct reac_roster_seg want[REAC_ROSTER_MAX];  /* what this tick derived */
	int n_want;
	unsigned overflow;                             /* adds past the bound, counted */
};

void reac_roster_init(struct reac_roster *r);

/* Start a tick's roster. The wanted set is emptied; `pub` — what the node carries — is
 * untouched until commit. */
void reac_roster_begin(struct reac_roster *r);

/* Add one segment to this tick's roster. Kept in BYTE order of `name`, because the index
 * is an ORDER and not an identity: a reader keys off `.name`, and an order derived from
 * the names alone cannot drift from the tables it was read out of. `model` NULL/"" reads
 * as `none`. Returns 0, or -1 when the table is full (counted in `overflow`) or the same
 * segment was added twice — which is a caller with two tables claiming one wire, never a
 * thing to swallow. */
int reac_roster_add(struct reac_roster *r, const char *name, enum reac_roster_state state,
                    const char *model, const char *role, const char *source, int in, int out);

/* WHAT MOVED, as properties to set or remove, in the grammar:
 *
 *   reac.roster            = 1                  (create-time, on the node itself)
 *   reac.roster.n          = 3
 *   reac.roster.<i>.name   = enp131s0.11
 *   reac.roster.<i>.state  = probing|established|slave|tap|refused|ignored
 *   reac.roster.<i>.model  = S-1608 | none
 *   reac.roster.<i>.role   = auto|master|slave|tap
 *   reac.roster.<i>.source = autodetected | conf:reac-pw.conf.d/50-openmixer.conf
 *   reac.roster.<i>.width  = 16/8 | 0/0
 *
 * Returns how many entries were written (0 = nothing changed, publish nothing), or -1 when
 * `max` is smaller than the delta needs — REFUSED, not truncated, because a roster
 * delivered short is a console reading a rig that is not there. Pass REAC_ROSTER_KV_MAX
 * and it can never happen. `out` is undefined on -1. */
int reac_roster_delta(struct reac_roster *r, struct reac_roster_kv *out, int max);

/* The delta was published: the wanted set becomes what the node carries. Never call this
 * for a delta that was not published, or the next tick will believe a lie. */
void reac_roster_commit(struct reac_roster *r);

#endif /* REAC_ROSTER_H */
