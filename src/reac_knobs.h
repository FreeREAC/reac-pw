// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knobs — every REACPW_ and REAC_ env/conf knob, DISCOVERED and PUBLISHED.
 *
 * "We should be able to set them and keep them if needed, and announce them when
 * detected, so that we can manage them; autodetection does not mean obscurity, it's
 * discovery and publish." (operator, 2026-09-17). Segment/role autodetection
 * (2026-09-16) removed the daemon's DEPENDENCY on env vars for what it decides for
 * itself; it never forbade an operator override for what is legitimately tunable, and
 * `reac_knobs_announce()` is what makes every such override VISIBLE at start, rather
 * than something only docs/ENV-KNOBS.md and --help claim exists.
 *
 * A knob's `conf_capable` bit means its REAL call site reads it through
 * `reac_conf_lookup` (this table's own lookup, in `reac_knobs_announce`, is the SAME
 * call — a different answer here from the real read site would be an announce that
 * lies). A knob marked NOT conf_capable is read with a bare `getenv` at its call site
 * and is announced the same way: never claimed to be layered when it is not.
 * `tests/test_reac_knob_table.c` and `tools/gen-env-knobs-doc.py` both walk this same
 * table, so the code, the announce and docs/ENV-KNOBS.md cannot drift apart silently. */
#ifndef REAC_KNOBS_H
#define REAC_KNOBS_H

#include <stdio.h>
#include <stddef.h>
#include <reac/transport/reac_conf.h>   /* enum reac_conf_layer */

struct reac_knob {
	const char *key;
	int conf_capable;
	const char *why_not_conf; /* NULL when conf_capable; else the reason, one line */
};

/* The closed table. Defined in reac_knobs.c so it exists exactly once. */
extern const struct reac_knob g_reac_knobs[];
extern const int g_reac_knobs_count;

/* One reading of a boolean knob THROUGH THE LAYERS -- reac_envflag.h's own parser, fed
 * from reac_conf_lookup instead of a bare getenv, so a boolean knob gets the same
 * reac-pw.env capability as every other one here. */
int reac_conf_flag(const char *key, int dflt);

/* THE COMMAND LINE (operator ruling, 2026-09-17): --set KEY=VALUE, highest precedence,
 * resolved against this same table. Returns 1 if `key` names a known knob (accepted),
 * 0 if not — the caller (main.c) refuses an unknown key with RC_E_UNKNOWN_KNOB and
 * exits rather than silently ignoring a typo. `key` and `value` must outlive the
 * process (they point into argv[], which does). */
int reac_knobs_set_argv(const char *key, const char *value);

/* `key` resolved at argv (reac_knobs_set_argv) > reac_conf_lookup's own precedence.
 * Same contract as reac_conf_lookup, plus REAC_CONF_ARGV when a --set answered. Every
 * knob site that used to call reac_conf_lookup directly for a table key calls this
 * instead, so a command-line override reaches it too. */
enum reac_conf_layer reac_knobs_resolve(const char *key, char *out, size_t cap);

/* THE SAME RESOLVE, ASKED ABOUT ONE PORT OR SEGMENT. `reac_knobs_resolve` passes NULL
 * for the segment, so it can never see a `<KEY>_<name>` layer; this one does — argv
 * (which is host-wide by construction: --set names a bare key) first, then
 * reac_conf_lookup's own precedence WITH the segment layer in it. `name` is whatever
 * that key is keyed by at its read site, and for REACPW_LINK_MBIT that is the PHYSICAL
 * port, never a VLAN child (#107). */
enum reac_conf_layer reac_knobs_resolve_port(const char *key, const char *name,
                                             char *out, size_t cap);

/* reac_conf_flag's own shape, through reac_knobs_resolve instead of reac_conf_lookup —
 * a boolean knob that also honours --set. */
int reac_knobs_resolve_flag(const char *key, int dflt);

/* One line per knob that is SET, one grammar, so an operator can see every override in
 * force without reading source: `reac-pw: S_KNOB_SET knob KEY=value (cli|env|conf)`.
 * Unset prints nothing. Writes to `out` (normally stderr; a test passes its own FILE*).
 * Returns the number of knobs found SET. */
int reac_knobs_announce(FILE *out);

#endif /* REAC_KNOBS_H */
