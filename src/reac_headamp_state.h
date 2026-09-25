// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_state — the READ side of the head-amp door: what this daemon is
 * asserting, and why a write was refused.
 *
 * docs/design/specs/2026-09-14-headamp-as-node-params.md §3a (RULED by the
 * operator 2026-09-14) closes the two gaps the write door left open:
 *
 *   1. A REFUSED WRITE WAS SILENT. A cell the parse dropped, and a cell written
 *      to a node with no preamp capability at all, both returned nothing: the
 *      caller saw a successful set-param and no audio moved. This module carries
 *      the refusal codes and the applied/unavailable state that answer it, the
 *      exact counterparts of `reac.cfg.rate.state` / `.refused`.
 *   2. THERE WAS NO READBACK. The wire has none — a box never re-broadcasts its
 *      head-amp state (mixer-protocol.md §6) — but reac-pw HOLDS the shadow
 *      table it re-pushes at every establishment, and published none of it, so a
 *      second client could not render a switch without inventing its own copy.
 *      `struct reac_headamp_asserted` is that table, rendered as one compact
 *      cell list.
 *
 * WHAT `asserted` IS AND IS NOT. It is what THIS DAEMON is putting on the wire.
 * It is never a report from the box, and the name is chosen so a reader cannot
 * believe otherwise. A cell is in it because we sent it, not because anything
 * confirmed it.
 *
 * THE REFUSAL IS A STANDING ANSWER, NOT ONLY A WRITE'S RECEIPT. `no-box`,
 * `box-master` and `no-base` are properties of the segment and stand whether or
 * not anybody has written yet — that is what lets a surface render *the box is
 * master, its preamps are preconfigured* instead of a dimmed control, before it
 * has tried a write it already knows will fail (spec §3c). `bad-key` and
 * `out-of-range` are the opposite: they are reachable ONLY by an actual write,
 * which is what makes them the two codes that prove the write path is alive.
 *
 * PURE: no PipeWire, no pacer, no libreac transport. Unit-testable offline. */
#ifndef REAC_HEADAMP_STATE_H
#define REAC_HEADAMP_STATE_H

#include <stddef.h>
#include <stdint.h>

#include <reac/reac_ctrl.h>   /* REAC_HEADAMP_MAX_CH, REAC_HEADAMP_SENS_MAX */
#include <reac/reac_ctrlblk.h> /* REAC_HEADAMP_NPARAMS */
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

/* The READ-side node properties, published on the SAME master sink node
 * (`reac-playback[.<segment>]`) that consumes the `reac.headamp.<ch>.<param>`
 * control keys, so one lookup yields the shape, the address, the travel, what is
 * asserted and why a write was refused. They are node properties, never
 * SPA_PROP_params, so they never collide with the control keys — see
 * reac_headamp_prop.h for the write side and for `reac.headamp.channels` /
 * `reac.headamp.caps`. */
#define REAC_PROP_HEADAMP_SENS_MAX "reac.headamp.sens.max" /* decimal, the travel's top step */
#define REAC_PROP_HEADAMP_ASSERTED "reac.headamp.asserted" /* "ch:param=value,..." ("" = none) */
#define REAC_PROP_HEADAMP_STATE    "reac.headamp.state"    /* applied | unavailable */
#define REAC_PROP_HEADAMP_REFUSED  "reac.headamp.refused"  /* a code below, or "none" */

/* `reac.headamp.state`. APPLIED means the segment carries preamps we can address
 * and the last write reached the send table; UNAVAILABLE means this segment has
 * no head-amp control on the wire at all, and the refusal code says which of the
 * three reasons it is. */
#define REAC_HEADAMP_STATE_APPLIED     "applied"
#define REAC_HEADAMP_STATE_UNAVAILABLE "unavailable"

/* Why a head-amp write was (or would be) refused. The first three are CAPABILITY
 * refusals — facts about the segment, standing with or without a write. The last
 * two are WRITE refusals, produced only by a cell that actually arrived. */
enum reac_headamp_refuse {
	REAC_HEADAMP_REFUSE_NONE = 0,
	REAC_HEADAMP_REFUSE_NO_BOX,        /* no model recognised: no preamps to address  */
	REAC_HEADAMP_REFUSE_BOX_MASTER,    /* the box is on M: no head-amp on this wire   */
	REAC_HEADAMP_REFUSE_NO_BASE,       /* box, but no announced strap: no wire address*/
	REAC_HEADAMP_REFUSE_BAD_KEY,       /* not a cell address: bad channel / param name*/
	REAC_HEADAMP_REFUSE_OUT_OF_RANGE,  /* addressed a cell, value/channel out of range*/
};

/* The short code for REAC_PROP_HEADAMP_REFUSED; "none" when nothing is refused.
 * Hyphenated, as spec §3a spells them. */
const char *reac_headamp_refuse_code(enum reac_headamp_refuse r);

/* THE CAPABILITY DECISION, pure and total.
 *
 *   box_master  nonzero on a segment a stagebox masters (reac-pw runs the slave
 *               engine there and there is no head-amp record on the wire in
 *               EITHER direction — measured: a byte-identical SET reached a box
 *               on M and the floor did not move, against +18.9 dB on an enrolled
 *               S-1608 by the same write).
 *   channels    preamp-capable box inputs; 0 until a model is recognised.
 *   base        the announced chassis strap, or < 0 for none. Never guessed: 0
 *               would address an S-1608's preamps 32 slots low and silently.
 *
 * Returns REFUSE_NONE when the segment can carry a head-amp record. `box_master`
 * outranks `no-box` because it is the more specific fact and the only one a
 * surface can turn into a sentence. */
enum reac_headamp_refuse reac_headamp_cfg_decide(int box_master, int channels, int base);

/* The `reac.headamp.state` answer for the same three facts: APPLIED when
 * reac_headamp_cfg_decide accepts, UNAVAILABLE otherwise. */
const char *reac_headamp_cfg_state(int box_master, int channels, int base);

/* WHAT THIS DAEMON IS ASSERTING. A mirror, on the MAIN LOOP, of the cells the
 * door has accepted and handed to the pacer's send table. It is not a second
 * ledger: the pacer's table is written from this same accepted-cell stream and
 * from nowhere else, so the two cannot disagree — this one exists because the
 * pacer's copy is owned by the RT thread and cannot be read to compose a
 * property. `set` is what distinguishes a deliberate 0 from an untouched cell,
 * exactly as reac_headamp_tx does. */
struct reac_headamp_asserted {
	uint8_t value[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];
	uint8_t set[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];
};

/* Empty the table (nothing asserted). */
void reac_headamp_asserted_init(struct reac_headamp_asserted *t);

/* Record one accepted cell. Returns 0, or -1 on a bad ch/param/value — the same
 * bounds reac_headamp_tx_set applies, so a cell this refuses is one the send
 * table would have refused too. */
int reac_headamp_asserted_set(struct reac_headamp_asserted *t, uint8_t ch,
                              uint8_t param, uint8_t value);

/* The longest string reac_headamp_asserted_render can produce, NUL included:
 * every cell as "<ch>:<param>=<value>" (2+1+1+1+2 = 7) plus a separator. */
#define REAC_HEADAMP_ASSERTED_MAX \
	(REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS * 8 + 1)

/* Render the set cells as the compact list spec §3a rules — "34:0=1,34:2=52",
 * ch:param=value in decimal, ascending by channel then param, and the empty
 * string when nothing is set. snprintf convention: returns the length that WOULD
 * have been written, and always NUL-terminates within buflen > 0. */
size_t reac_headamp_asserted_render(const struct reac_headamp_asserted *t,
                                    char *buf, size_t buflen);

#endif /* REAC_HEADAMP_STATE_H */
