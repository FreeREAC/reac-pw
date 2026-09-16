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
#define REAC_SEGCONF_REFUSAL_LEN 200

/* The file, relative to `~/.config/reac-pw/`. Stated here so the daemon, the test and the
 * documentation cannot drift apart. */
#define REAC_SEGCONF_DIR  ".config/reac-pw"
#define REAC_SEGCONF_FILE "reac-pw.conf"

/* THE DROP-IN DIRECTORY, and the door the console writes through (spec amendment
 * 2026-09-16 third, §A). Same grammar, same parser, same vocabulary; only the ORDER is
 * new — the hand-written file first, then every `*.conf` here in BYTE order, LAST WINS
 * PER KEY. That is the order systemd, sysctl.d, udev and WirePlumber already train every
 * operator on this host to expect, and it is what lets BOTH doors the operator ruled for
 * actually override: the console writes `reac-pw.conf.d/50-openmixer.conf` and never
 * touches the hand-written file, and an operator who wants the last word takes it back
 * with a name that sorts later (`99-local.conf`). It is safe only because every answer
 * names the file it came from — see reac_segconf_role_file below. */
#define REAC_SEGCONF_DIRD "reac-pw.conf.d"

/* The base file plus this many drop-ins. Past it the excess is REPORTED, never read in
 * silence — a console that writes one file and an operator who writes a handful are the
 * whole population, and a directory with 17 of them is a mistake worth naming. */
#define REAC_SEGCONF_FILES 17
/* `reac-pw.conf` or `reac-pw.conf.d/<name>` — what a refusal and a provenance answer
 * carry. Never an absolute path: what the operator needs is which of THEIR files said it,
 * and the directory is already printed once at start. */
#define REAC_SEGCONF_FILE_LEN 96

/* A box-model token: the longest in the table is `s4000s-0832`. A token past this is
 * refused by name like any other unknown one. */
#define REAC_SEGCONF_MODEL_LEN 24

struct reac_segconf_seg {
	char name[IFNAMSIZ];
	enum reac_role_intent role;  /* meaningful only when role_set */
	int  role_set;               /* SOME file said `role =` for this segment */
	int  ignore;                 /* the last file that said `ignore =` said yes */
	int  ignore_set;             /* some file said `ignore =` at all */
	int  said_ignored;           /* the "not sniffed" line is printed once per segment */
	/* THE MODEL ROW A BOX-ROLE SEGMENT DECLARES (2026-09-17 spec §4). A token from
	 * libreac's box-model table — `s1608`, `s4000s-0832`, `fr4000` — and nothing
	 * else: what we present to a mixer is a declared row, never a width somebody
	 * typed. Meaningful ONLY under `role = box`; on any other role it is refused by
	 * name, because a key that is read on one role and ignored on another is a trap
	 * with no upside. There is NO DEFAULT: a box that declares the wrong width to a
	 * mixer is a patch that silently lands on the wrong channels. */
	char model[REAC_SEGCONF_MODEL_LEN];
	int  model_set;
	char model_file[REAC_SEGCONF_FILE_LEN];
	/* WHICH FILE ANSWERED, PER KEY — the provenance the amendment's §C requires, and the
	 * thing that makes last-wins debuggable rather than merely defined. Per KEY and not
	 * per section, because last-wins is per key: `ignore` from the operator's file and
	 * `role` from the console's drop-in is the ordinary case, not a corner one. */
	char role_file[REAC_SEGCONF_FILE_LEN];
	char ignore_file[REAC_SEGCONF_FILE_LEN];
	unsigned seen_gen;           /* the file-number this section was last opened in */
};

/* What a file looked like when it was read, for reac_segconf_refresh. NANOSECOND mtime:
 * a seconds-resolution stamp cannot see a drop-in rewritten twice in one second, which is
 * exactly what a console writing its file does. */
struct reac_segconf_stamp {
	long long mtime_ns;
	long long size;
	unsigned long long ino;
	int present;
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
	/* EVERY FILE READ, IN READ ORDER, so the start block can print the order it obeyed.
	 * `file[0]` is REAC_SEGCONF_FILE whenever the hand-written file was there. */
	char file[REAC_SEGCONF_FILES][REAC_SEGCONF_FILE_LEN];
	int n_files;
	unsigned files_overflow;     /* drop-ins past REAC_SEGCONF_FILES */
	/* What each of them looked like when it was read, plus the DIRECTORY — a file added
	 * or removed moves the directory's own mtime and nothing else. */
	struct reac_segconf_stamp stamp[REAC_SEGCONF_FILES];
	struct reac_segconf_stamp stamp_dir;
	char home[256];              /* what _load was given, so a refresh can repeat it */
	char base[512];              /* `<home>/.config/reac-pw`: every file path is base/file[i] */
	char reading[REAC_SEGCONF_FILE_LEN];  /* the file being parsed, for the refusals */
	unsigned gen;                /* the file-number being parsed; a repeated section is a
	                              * typo WITHIN one file and an override ACROSS two */
};

void reac_segconf_init(struct reac_segconf *c);

/* Parse one file's whole TEXT, as if it were `label` (`reac-pw.conf` or
 * `reac-pw.conf.d/<name>`), MERGING onto whatever earlier files said: a key this text
 * carries overrides, a key it is silent about keeps its earlier answer. Returns the
 * running number of segments (0 is a perfectly good answer for an empty or all-comment
 * file). Never fails: everything it cannot use is refused by NAME AND FILE into
 * `refusal[]`. */
int reac_segconf_parse_file(struct reac_segconf *c, const char *text, const char *label);

/* The same, as the hand-written file. Kept because most callers have one text and no
 * directory, and because a test of the GRAMMAR should not have to name a file. */
int reac_segconf_parse(struct reac_segconf *c, const char *text);

/* Read `<home>/.config/reac-pw/reac-pw.conf`, THEN every `*.conf` in
 * `<home>/.config/reac-pw/reac-pw.conf.d/` in byte order, and parse them in that order —
 * later wins per key (REAC_SEGCONF_DIRD above has the reasons). `home` NULL means $HOME,
 * the same convention reac_conf_lookup uses. `path` is filled in either way. Returns the
 * number of segments; an absent hand-written file returns 0 with `present` 0 and no
 * refusal, and an absent directory is equally normal. A drop-in that exists and cannot be
 * READ is a refusal by name, and the files that could be read are still honoured. */
int reac_segconf_load(struct reac_segconf *c, const char *home);

/* RE-READ IF ANYTHING HAS MOVED UNDER US. A stat() of the conf, a stat() of the
 * directory, and — only when neither moved — one of each drop-in that was read; a reload
 * only when an mtime, size or inode changed, or a file or the directory appeared or
 * vanished. A file ADDED or REMOVED moves the directory's own mtime, so the common case
 * costs two stats and nothing is opened unless something moved. Returns 1 if it reloaded.
 *
 * WHY ON DEMAND AND NOT ONCE AT START. Every other layer this daemon's configuration has
 * is read at the moment it is asked (`reac_conf_lookup` opens its files on every call),
 * and a NEW file with a different lifetime from all of them is the kind of inconsistency
 * nobody remembers at 2 a.m. A segment's role is resolved when its sniffer opens and when
 * its listener opens — link-up, hot-plug, a re-link — so this is a handful of stats per
 * event and never one per frame.
 *
 * THIS IS NOT THE LIVE ROLE CHANGE. A segment already running keeps the engine it opened
 * with; what re-reads here is what the NEXT resolution sees. The live path is
 * `reac.cfg.role` on the segment's own door, and the SIGHUP re-election of
 * 2026-09-16-auto-role-per-segment.md §5b, neither of which this replaces. */
int reac_segconf_refresh(struct reac_segconf *c);

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

/* WHICH FILE PINNED THIS SEGMENT'S ROLE — `reac-pw.conf` or `reac-pw.conf.d/<name>` — or
 * NULL when nothing did (the normal case: the wire decided). This is what
 * `reac.roster.<i>.source` publishes and what the start block prints, and it is the whole
 * safety of last-wins: an override nobody can trace back to a file is the 2026-09-16
 * fault with one more file in it. */
const char *reac_segconf_role_file(const struct reac_segconf *c, const char *name);

/* The same for `ignore`, so an IGNORED segment on the roster can say who switched it
 * off. NULL when no file said `ignore =` for it. */
const char *reac_segconf_ignore_file(const struct reac_segconf *c, const char *name);

/* THE MODEL ROW THIS SEGMENT DECLARES, or NULL when it declares none — which is every
 * segment that is not a box, and a box whose model was refused. The string is a token of
 * libreac's own box-model table (reac_box_model_by_token), folded to lower case, so the
 * caller looks the row up rather than re-deriving a width from a name. */
const char *reac_segconf_model(const struct reac_segconf *c, const char *name);

/* Every VLAN segment this file DECLARES, appended to a reac_declared_vlan table: naming
 * `[segment <parent>.<vid>]` is the declaration, so the netdev is minted at start whether
 * or not anything has ever been heard on it (spec §3a; reac_declared_vlan.h's cold-boot
 * reason). An IGNORED segment declares nothing — switching a segment off cannot create a
 * netdev for it. Returns how many NEW entries were added, or -1 if the table filled. */
struct reac_declared_vlan;
int reac_segconf_declared(const struct reac_segconf *c, struct reac_declared_vlan *tab,
                          int max, int *n);

#endif /* REAC_SEGCONF_H */
