// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rate_cfg — the REAC pace: a CLOSED per-protocol list, DECLARED here;
 * drivability is a separate, OBSERVED fact
 * (docs/design/specs/2026-08-26-reac-runtime-config.md §0, in the openmixer
 * tree). REAC has exactly three rates by definition: 44100, 48000, 96000 Hz.
 * This module is the one place that list is spelled out for reac-pw today;
 * the shared cross-repo vocabulary table (a libreac header both sides pin
 * against) is the spec's own increment 2, not this one.
 *
 * PURE decision core, no I/O, no PipeWire — same shape as reac_master and
 * reac_headamp_prop: unit-testable offline. Two callers:
 *   - reac_sink_node's param_changed, to decide a `reac.cfg.rate` assertion
 *     (the RT-owning pacer only ever APPLIES an already-accepted rate — see
 *     reac_pacer_request_rate).
 *   - main.c, to pick the standing default when no assertion exists.
 *
 * HONESTY (the spec's own clause): reac-pw has no real drivability probe yet —
 * no PHC-presence check, no pacing-headroom measurement. Production code
 * therefore always passes REAC_RATE_ALL_BITS as the drivable mask: an honest
 * declaration of "we assume everything we don't yet measure", never a guessed
 * number dressed up as a measurement. A narrower mask exists in this module's
 * API purely so the DEFAULT-IS-THE-BEST-DRIVABLE rule has something to prove
 * without inventing a fake probe — a test constructs a mask directly, the way
 * a future probe's result would arrive, and checks the arithmetic that probe
 * would feed. What a real probe would measure: PHC presence on the TX NIC,
 * and whether this host's clock_nanosleep cadence has enough headroom at
 * 96 kHz's 125 us slot to avoid the depth-guard discards docs/FASTPATH-
 * MEASUREMENTS.md and docs/96K-SWITCH-ASSESSMENT.md already describe. */
#ifndef REAC_RATE_CFG_H
#define REAC_RATE_CFG_H

#include <stddef.h>
#include <string.h>

#include "reac_role.h"

struct spa_pod;

/* The cfg namespace key this increment adds. The spec's §1 names the general
 * "reac.cfg.*" namespace, handled where "reac.headamp.*" already is. */
#define REAC_CFG_PROP_RATE "reac.cfg.rate"

/* Published, read-side props (spec §0/§1): the standing rate, whether it came
 * from an operator assertion or the no-assertion default, the drivable
 * subset, whether a re-establish triggered by a rate change is still in
 * flight, and a refusal code for the last `reac.cfg.rate` write. "none" is
 * this codebase's established sentinel for "no value applies" (see
 * reac.master.mac in reac_link_state.h). */
#define REAC_PROP_RATE           "reac.rate"
#define REAC_PROP_RATE_SOURCE    "reac.rate.source"      /* "asserted" | "convention" */
#define REAC_PROP_RATE_DRIVABLE  "reac.rate.drivable"     /* csv, ascending         */
#define REAC_PROP_RATE_STATE     "reac.cfg.rate.state"    /* "applied" | "pending"  */
#define REAC_PROP_RATE_REFUSED   "reac.cfg.rate.refused"  /* code, or "none"        */

/* THE FAMILY knob (2026-08-26-reac-runtime-config.md §4/§5): the emulated console
 * GENERATION, asserted by the console and applied by re-establishing the segment
 * (cfea[19] is set at establishment, so it cannot flip live — same as rate). Its
 * OPTIONS are the closed family set; changing it re-evaluates the rate to the best
 * drivable under the NEW family (operator ruling 2026-08-26). */
#define REAC_CFG_PROP_CONSOLE     "reac.cfg.console"     /* write door         */
#define REAC_PROP_CONSOLE         "reac.console"         /* standing family    */
#define REAC_PROP_CONSOLE_OPTIONS "reac.console.options" /* csv, the OPTIONS    */
#define REAC_CONSOLE_VMIXER "vmixer"   /* console_field 0 = M-200 / M-300 */
#define REAC_CONSOLE_OHRCA  "ohrca"    /* console_field 1 = M-5000        */
#define REAC_CONSOLE_OPTIONS_CSV (REAC_CONSOLE_VMIXER "," REAC_CONSOLE_OHRCA)

/* token -> console_field (0/1), or -1 if the string names no known family. */
static inline int reac_console_family_parse(const char *s)
{
	if (!s) return -1;
	if (strcmp(s, REAC_CONSOLE_VMIXER) == 0) return 0;
	if (strcmp(s, REAC_CONSOLE_OHRCA)  == 0) return 1;
	return -1;
}
/* console_field -> stable token (nonzero = OHRCA, the 96k-capable generation). */
static inline const char *reac_console_family_name(unsigned console_field)
{
	return console_field ? REAC_CONSOLE_OHRCA : REAC_CONSOLE_VMIXER;
}

/* Parse a `reac.cfg.console` family assertion out of a Props object pod, as
 * reac_rate_prop_parse does for the rate: 1 + *out_cf (0/1) on a known family,
 * -1 if the key was present but the value unusable, 0 if none was carried. */
int reac_console_prop_parse(const struct spa_pod *props, int *out_cf);

#define REAC_RATE_SOURCE_ASSERTED "asserted"
/* Operator, 2026-08-26: "default is not a valid value — we make the best the default,
 * it is a convention." A rate the operator gave (--rate, a conf file, a console
 * assertion over the graph) is ASSERTED; with no assertion standing the daemon runs
 * the best drivable rate BY CONVENTION, and that is what this value says. */
#define REAC_RATE_SOURCE_CONVENTION "convention"
#define REAC_RATE_STATE_APPLIED   "applied"
#define REAC_RATE_STATE_PENDING   "pending"

/* One bit per closed-list rate. */
#define REAC_RATE_BIT_44100  (1u << 0)
#define REAC_RATE_BIT_48000  (1u << 1)
#define REAC_RATE_BIT_96000  (1u << 2)
#define REAC_RATE_ALL_BITS   (REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000 | REAC_RATE_BIT_96000)

/* Why a `reac.cfg.rate` assertion was refused. REFUSE_NONE doubles as the
 * published state once a refusal is superseded by an accepted rate. */
enum reac_rate_refuse {
	REAC_RATE_REFUSE_NONE = 0,
	REAC_RATE_REFUSE_NOT_CLOSED,    /* not one of 44100 / 48000 / 96000           */
	REAC_RATE_REFUSE_NOT_DRIVABLE,  /* in the closed list, outside this segment's */
	REAC_RATE_REFUSE_ROLE_SLAVE,    /* a slave has no rate setting of its own     */
	REAC_RATE_REFUSE_MALFORMED,     /* the prop value was not a usable number     */
};

/* Short code for REAC_PROP_RATE_REFUSED; "none" when nothing is refused. */
const char *reac_rate_refuse_code(enum reac_rate_refuse r);

/* Is hz one of the three REAC rates? */
int reac_rate_is_closed(int hz);

/* This rate's bit in a drivable_mask, or 0 if hz is not in the closed list at
 * all (so a caller can never test a bit that does not exist). */
unsigned reac_rate_bit(int hz);

/* The rate ceiling the emulated console GENERATION imposes, as a drivable-mask.
 * VERIFIED on hardware 2026-08-26: only an OHRCA master (M-5000, console_field 1)
 * drives a box at 96 kHz; a V-Mixer master (M-200 / M-300, console_field 0) caps
 * it at 48 kHz — so the family, not just the NIC, bounds what this segment can
 * pace. A DECLARED cap (a property of the family, like the closed list itself),
 * meant to be ANDed into the observed drivable mask so the published subset, the
 * default pick and the refusal all agree: a V-Mixer segment never offers,
 * defaults to, or accepts 96 kHz. cfea[19] stays the family byte — this is the
 * one place family and rate legitimately meet, as a CEILING, never by deriving
 * one byte from the other (see reac-pw's rate-family-orthogonality gate). */
static inline unsigned reac_rate_family_mask(unsigned console_field)
{
	/* OHRCA (1) drives all three; a V-Mixer (0) tops out at 48 kHz. */
	return console_field ? REAC_RATE_ALL_BITS
	                     : (REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000);
}

/* The highest rate whose bit is set, 0 if mask carries none of the closed
 * three (an honest probe, or the all-bits default, never produces this — a
 * segment that can drive nothing is not a segment). */
int reac_rate_best_drivable(unsigned drivable_mask);

/* Render the drivable subset as an ascending CSV ("44100,48000,96000" style,
 * narrowed to whatever `mask` sets). snprintf convention: returns the length
 * that would have been written, and always NUL-terminates within buflen>0. */
size_t reac_rate_drivable_csv(unsigned drivable_mask, char *buf, size_t buflen);

/* THE decision: accept or refuse a `reac.cfg.rate` assertion of
 * `requested_hz` for a daemon in role `role` whose segment can drive
 * `drivable_mask`. Pure — makes no state change; the caller applies the
 * change only on REAC_RATE_REFUSE_NONE. */
enum reac_rate_refuse reac_rate_cfg_decide(enum reac_role role, int requested_hz,
                                           unsigned drivable_mask);

/* Parse a `reac.cfg.rate` entry out of a SPA_PARAM_Props object pod's
 * SPA_PROP_params list (the same carrier reac_headamp_prop reads — see its
 * header for why: SPA has no standard id for a vendor rate knob). Accepted as
 * Int or Float so any controller's natural encoding works, same tolerance as
 * head-amp. Returns 1 and sets *out_hz if the key was present and parseable,
 * 0 if the key is simply absent (a Props write that only touched volume or
 * head-amp), -1 if the key was present but its value could not be read as a
 * number, or if `props` is not an Object pod at all. PURE: no PipeWire, no
 * pacer — unit-testable offline. */
int reac_rate_prop_parse(const struct spa_pod *props, int *out_hz);

#endif /* REAC_RATE_CFG_H */
