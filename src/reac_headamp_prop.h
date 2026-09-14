// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_prop — parse LIVE head-amp control changes out of a PipeWire
 * SPA_PARAM_Props object (task #203).
 *
 * A controller (openmixer) drives the master node's box preamps at runtime the
 * same way it drives volume/mute: by SETTING SPA_PARAM_Props on the node. There
 * is no standard SPA_PROP_* id for per-channel phantom/pad/sens, so the changes
 * ride SPA_PROP_params — SPA's extensible "simple control params" carrier, a
 * Struct of alternating (String key, Pod value) pairs (see spa/param/props.h).
 * The keys are "reac.headamp.<ch>.<phantom|pad|sens>" and the value is the
 * absolute setting (0/1 for phantom+pad, 0..0x37 for sens; accepted as Bool,
 * Int, or Float so any controller's natural encoding works).
 *
 * This module is PURE: it only touches the SPA pod (no PipeWire, no filter, no
 * pacer), so it is unit-testable offline by building a Props pod with
 * spa_pod_builder. The sink node calls it from param_changed and forwards each
 * parsed setting to reac_pacer_headamp_set (the lock-free handoff to the RT
 * pacer). Keeping the parse separate from the handoff is what lets both be
 * tested without a live graph. */
#ifndef REAC_HEADAMP_PROP_H
#define REAC_HEADAMP_PROP_H

#include <reac/reac_headamp_tx.h>   /* struct reac_headamp_setting */
#include "reac_headamp_state.h"   /* enum reac_headamp_refuse — the answer a refusal gets */

struct spa_pod;

/* The per-channel head-amp key prefix inside SPA_PROP_params. A full key is
 * REAC_HEADAMP_PROP_PREFIX "<ch>." "<param-name>", e.g. "reac.headamp.3.phantom". */
#define REAC_HEADAMP_PROP_PREFIX "reac.headamp."

/* Head-amp CAPABILITY keys — the READ side of the same contract, published as
 * NODE PROPERTIES (pw_properties, not SPA_PROP_params) on the SAME node that
 * consumes the control keys above, so a consumer discovers the box's preamp
 * shape and drives it on one node (task #205). Both are self-describing and
 * driven by the recognized box model:
 *
 *   reac.headamp.channels — decimal count of preamp-capable box INPUTS. "0" until
 *                           a model is recognized; then the model's input width
 *                           (e.g. "16" for an S-1608, "8" for an S-0808).
 *   reac.headamp.caps     — comma-separated preamp capabilities the box carries.
 *                           "phantom,pad,sens" is the trio every current REAC
 *                           stagebox preamp exposes; a model that differs would
 *                           publish its own honest subset/superset here.
 *
 * These live in the node-property dict and never ride SPA_PROP_params, so they
 * are a different namespace from the control keys and reac_headamp_prop_parse
 * never sees them (its channel parse rejects the non-numeric "channels"/"caps"
 * tails anyway). A reader that finds the control PropInfo but NOT these keys is
 * talking to a reac-pw that predates capability publication. */
#define REAC_PROP_HEADAMP_CHANNELS "reac.headamp.channels"
#define REAC_PROP_HEADAMP_CAPS     "reac.headamp.caps"

/* The capability set every REAC stagebox preamp in the fixed model matrix
 * carries today (phantom 48V, -20 dB pad, 1 dB/step sensitivity). Seeded at
 * node create and left standing; re-encode per-model here only if a future
 * model's preamp genuinely differs. */
#define REAC_HEADAMP_CAPS_DEFAULT  "phantom,pad,sens"

/* WHAT A PARSE SAW, beyond what it accepted.
 *
 * `n` alone cannot tell "this Props object carried no head-amp key at all" from
 * "it carried one and the parse dropped it" — both are 0, and that indistinction
 * IS the silent-refusal gap spec §2 names. `keys` counts every key under the
 * head-amp prefix, good or bad, so the caller knows a write was attempted at all;
 * `refusal` is the FIRST thing wrong with one of them (a later good cell does not
 * erase an earlier bad one — a caller that reported "none" after dropping a cell
 * would be reporting the blind pass this exists to stop).
 *
 * The two write refusals, and the line between them: BAD_KEY means the key does
 * not address a cell (a non-numeric channel, no '.' after it, an unknown param
 * name); OUT_OF_RANGE means it addresses a cell but the channel is past
 * REAC_HEADAMP_MAX_CH or the value is not one the range admits — which includes a
 * value pod that is not a usable number at all, because there is no number
 * outside the range and no number in it either. */
struct reac_headamp_prop_result {
	int n;                              /* cells written to `out`                    */
	int keys;                           /* head-amp keys seen, accepted or refused   */
	enum reac_headamp_refuse refusal;   /* the FIRST refusal; NONE if every key stood */
};

/* Parse every "reac.headamp.<ch>.<param>" entry carried in `props` (a
 * SPA_PARAM_Props object pod)'s SPA_PROP_params list into `out` (capacity `max`),
 * and report what it saw in `res` (may be NULL). Keys that are not head-amp keys
 * are skipped without comment, so a mixed Props object with volume + a head-amp
 * change is fine. Returns the number of settings written (0..max), or -1 if
 * `props` is NULL / not an Object pod. Never writes past `max`. */
int reac_headamp_prop_parse_result(const struct spa_pod *props,
                                   struct reac_headamp_setting *out, int max,
                                   struct reac_headamp_prop_result *res);

/* The same parse, discarding what it saw. */
int reac_headamp_prop_parse(const struct spa_pod *props,
                            struct reac_headamp_setting *out, int max);

#endif /* REAC_HEADAMP_PROP_H */
