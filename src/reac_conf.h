// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_conf — the LAYERED configuration lookup, and the name of the layer that
 * answered.
 *
 * THE CONFIG IS LAYERED, NOT DUPLICATED. Two files carrying the same key is a
 * second ledger only when they sit at the SAME level. Here they do not: each is
 * a layer in a declared precedence, and a layer is a default the layer above is
 * free to override. The defect a layered config actually has is a different one
 * — an override order nobody wrote down, because then every reader infers a
 * different one and they are all sure they are right. So the order is declared
 * HERE, in the code that implements it, and in docs/RATE-AND-CLOCK-CONFIG.md,
 * and the two must agree.
 *
 * PRECEDENCE, HIGHEST FIRST:
 *
 *   1. THE COMMAND LINE            --rate, --live, ...
 *      Handled by main.c, not here: an explicit argument is not a lookup. It is
 *      the layer openmixer uses — the console owns the desk's configuration and
 *      hands it over when it launches us, which is what "omx needs to override"
 *      means in practice.
 *
 *   2. THE PROCESS ENVIRONMENT     REAC_RATE=..., REACPW_*
 *      An operator's ad-hoc override for one run, and the channel systemd's
 *      EnvironmentFile= delivers on. Above the files because a variable set for
 *      THIS invocation is more specific than a file that describes every one.
 *
 *   3. <KEY>_<segment>                     PER-SEGMENT
 *      The rig has two segments and they are not interchangeable: different
 *      boxes, different NICs, potentially different rates. A per-segment fact
 *      is the key SUFFIXED with the segment's name — REAC_ROLE_enp131s0 — in
 *      any of the layers below, and it outranks the bare key in every one of
 *      them. Segments are discovered, not declared (openmixer's
 *      2026-08-23-reac-trunk-vlan-daemon.md, amendment 2026-09-02), so there
 *      is no per-segment FILE to create: a segment's name is its interface's,
 *      and the console generates the key into reac-pw.env.
 *
 *   4. ~/.config/reac-pw/reac-pw.env       PER-HOST, all segments
 *      What every segment on this host shares.
 *
 *   5. ~/.config/openmixer/reac.env        LAST RESORT
 *      The bottom layer. It is what makes a STANDALONE install work with no
 *      console present, and openmixer overrides it and surfaces the value to the
 *      user. It is NOT a stray duplicate and it is NOT to be deleted; it is the
 *      floor of the stack, and a floor is the thing you land on when nothing
 *      above answered.
 *
 *   6. THE BUILT-IN DEFAULT        e.g. reac_rate_best_drivable() (reac_rate_cfg.h)
 *      Compiled in, or computed with no live input. Reached only when all
 *      five above are silent.
 *
 * An empty value is NOT an answer. `REAC_RATE=` sets nothing and falls through,
 * because a key someone blanked out is a key they turned off, not a key they set
 * to the empty string.
 *
 * File format is the shell-ish one systemd's EnvironmentFile= already accepts and
 * the existing files already use: `KEY=VALUE` one per line, `#` comments, blank
 * lines ignored, optional single or double quotes around the value. It is
 * deliberately NOT a shell: no expansion, no substitution, no command execution.
 * These files are read by a process holding CAP_NET_RAW. */
#ifndef REAC_CONF_H
#define REAC_CONF_H

#include <stddef.h>

/* Which layer answered. Reported to the operator verbatim, because a layered
 * config that cannot tell you which layer answered is a debugging trap — and
 * this rig has already spent a morning on exactly that class of confusion. */
enum reac_conf_layer {
	REAC_CONF_NONE = 0,     /* nothing answered; the caller's built-in wins */
	REAC_CONF_ARGV,         /* set by main.c when a command-line flag won */
	REAC_CONF_ENV,          /* the process environment */
	REAC_CONF_SEGMENT,      /* <KEY>_<segment>, in the environment or a conf file */
	REAC_CONF_HOST,         /* ~/.config/reac-pw/reac-pw.env */
	REAC_CONF_LAST_RESORT,  /* ~/.config/openmixer/reac.env */
	REAC_CONF_BUILTIN,      /* the compiled-in default */
};

/* A short human phrase for the layer, including WHICH FILE where there is one.
 * Never NULL. */
const char *reac_conf_layer_name(enum reac_conf_layer l);

/* Resolve `key` through the layers above (2 through 5 — argv is the caller's).
 * `segment` selects the per-segment key and may be NULL, which skips layer 3.
 * On a hit, copies the value into `out` (always NUL-terminated) and returns the
 * layer. On no hit anywhere, leaves `out` untouched and returns REAC_CONF_NONE.
 *
 * `home` is the base for ~ and exists so the test can point the whole stack at a
 * temporary directory; pass NULL for the real $HOME. */
enum reac_conf_layer reac_conf_lookup(const char *key, const char *segment,
                                      const char *home, char *out, size_t cap);

/* Read one key out of one env-format file. Exposed for the test and for anyone
 * who needs a single layer. Returns 1 on a hit, 0 on miss or unreadable file. */
int reac_conf_read_file(const char *path, const char *key, char *out, size_t cap);

#endif /* REAC_CONF_H */
