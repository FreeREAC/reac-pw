/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_segconf — the operator's ONE override file, and the only thing that can pin a
 * segment. (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md §3.)
 *
 * THE FAULT THIS EXISTS FOR, MEASURED ON THE DESK 2026-09-16. The rig moved from one box
 * on a 100 Mbit USB NIC to three boxes on a 1 Gbit trunk. The console's GENERATED
 * `~/.config/reac-pw/reac-pw.env` still carried the previous rig's answers, and those
 * answers were now wrong: the VLAN segments were pinned `tap`, so the daemon served three
 * mirror ports on a fabric that had no mirror. Every segment was up, every node was on the
 * graph, every counter was green, and no audio moved. A launch-time key is a decision taken
 * before there is anything to decide against, and from inside the daemon a stale file and a
 * correct one are indistinguishable.
 *
 * SO THE DEFAULT IS THE WIRE, AND THE FILE IS THE EXCEPTION. `REAC_ROLE` and
 * `REAC_ROLE_<segment>` are RETIRED as role sources in every layer (spec §2); a segment
 * with nothing said about it is `auto` and the hunt elects it. This file is the only thing
 * that overrides that, and it is HAND-WRITTEN: the console never generates it, because the
 * generator's idea of the rig is exactly the thing that goes stale.
 *
 * WHY NOT ANOTHER KEY IN reac-pw.env. Three reasons, each already paid for. (1) That file
 * is GENERATED — one store, one writer, and an override living in the generated file is
 * rewritten by the generator that was wrong. (2) The `<KEY>_<segment>` suffix splits at the
 * last underscore (reac_declared_vlan.h), so it cannot name an interface that contains one.
 * (3) `ignore` has no value to carry in a key whose name IS the fact — naming a segment in
 * order to switch it off is exactly the shape that put three declared master VLANs on a
 * 100 Mbit port with no box on any of them.
 *
 * REPORT, NEVER SILENTLY DROP, AND NEVER DIE. A typo in this file must be visible and must
 * not take a desk down mid-show. Both at once means every unusable line is REFUSED BY NAME
 * into `refusal[]` (the caller prints them at start) and the rest of the file is honoured.
 * A file that does not exist is not an error and not a warning: it is the normal case, and
 * `present` tells the caller which it was, because "absent" and "empty" must not read alike.
 *
 * PURE PARSER. No I/O below reac_segconf_load, no netlink, no PipeWire, no engine — the
 * whole grammar is unit-testable offline (tests/test_reac_segconf.c). */
#ifndef REAC_SEGCONF_H
#define REAC_SEGCONF_H

#include <net/if.h>   /* IFNAMSIZ */
#include <stddef.h>

#include <reac/reac_role.h>

/* Segments the file may name. Two trunks' worth at reac_topo's own per-parent bound, the
 * same number reac_declared_vlan carries; past it the excess is REPORTED, never dropped in
 * silence. */
#define REAC_SEGCONF_MAX 32

/* Refusals kept verbatim for the start-up block. Past this they are still COUNTED — a
 * bound that is silently hit is a bound nobody can act on. */
#define REAC_SEGCONF_REFUSALS 8
#define REAC_SEGCONF_REFUSAL_LEN 160

/* The file, relative to `~/.config/reac-pw/`. Stated here so the daemon, the test and the
 * documentation cannot drift apart. */
#define REAC_SEGCONF_DIR  ".config/reac-pw"
#define REAC_SEGCONF_FILE "reac-pw.conf"

struct reac_segconf_seg {
	char name[IFNAMSIZ];
	enum reac_role_intent role;  /* meaningful only when role_set */
	int  role_set;               /* the file said `role =` for this segment */
	int  ignore;                 /* the file said `ignore = yes` */
	int  said_ignored;           /* the "not sniffed" line is printed once per segment */
};

struct reac_segconf {
	struct reac_segconf_seg seg[REAC_SEGCONF_MAX];
	int n;
	int present;                 /* the file existed and was readable */
	char path[512];              /* where we looked, printed whether or not it was there */
	char refusal[REAC_SEGCONF_REFUSALS][REAC_SEGCONF_REFUSAL_LEN];
	int n_refusals;              /* how many are kept in `refusal` */
	unsigned refused;            /* how many there were in total */
	unsigned overflow;           /* segments past REAC_SEGCONF_MAX */
};

void reac_segconf_init(struct reac_segconf *c);

/* Parse one file's whole TEXT. Returns the number of segments the file names (0 is a
 * perfectly good answer for an empty or all-comment file). Never fails: everything it
 * cannot use is refused by name into `refusal[]`. */
int reac_segconf_parse(struct reac_segconf *c, const char *text);

/* Read `<home>/.config/reac-pw/reac-pw.conf` and parse it. `home` NULL means $HOME, the
 * same convention reac_conf_lookup uses. `path` is filled in either way. Returns the
 * number of segments; an absent file returns 0 with `present` 0 and no refusal. */
int reac_segconf_load(struct reac_segconf *c, const char *home);

/* The entry for `name`, or NULL. Segment names are interface names and are compared
 * case-SENSITIVELY, because that is how the kernel compares them. */
const struct reac_segconf_seg *reac_segconf_find(const struct reac_segconf *c,
                                                 const char *name);

/* Is this segment switched off? Never sniffed, never served, never minted, its netdev left
 * exactly as found. */
int reac_segconf_ignored(const struct reac_segconf *c, const char *name);

/* Did the file pin a role for this segment? Returns 1 and sets *out when it did — INCLUDING
 * an explicit `role = auto`, which is legal and says the same thing as silence; the caller
 * treats AUTO as no pin and may still report that the file said it. Returns 0 when the file
 * is silent about this segment, which is the normal case and means: the wire decides. */
int reac_segconf_role(const struct reac_segconf *c, const char *name,
                      enum reac_role_intent *out);

/* Every VLAN segment this file DECLARES, appended to a reac_declared_vlan table: naming
 * `[segment <parent>.<vid>]` is the declaration, so the netdev is minted at start whether
 * or not anything has ever been heard on it (spec §3a; reac_declared_vlan.h's cold-boot
 * reason). An IGNORED segment declares nothing — switching a segment off cannot create a
 * netdev for it. Returns how many NEW entries were added, or -1 if the table filled. */
struct reac_declared_vlan;
int reac_segconf_declared(const struct reac_segconf *c, struct reac_declared_vlan *tab,
                          int max, int *n);

#endif /* REAC_SEGCONF_H */
